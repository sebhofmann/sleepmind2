//! SleepMind deep NNUE trainer (NNUE_ARCH=deep in the engine).
//!
//! Architecture (PSQT only, adapted from bullet's `examples/advanced`, without threat/pawn-pair inputs):
//!   (768 x 16 king buckets, mirrored, + factoriser -> 768)
//!   -> per perspective: crelu(acc[0..384]) * crelu(acc[384..768])      (pairwise, 384)
//!   -> concat(stm, ntm) = 768 -> L1 (768 -> 16, output-bucketed)
//!   -> concat(x, x^2).crelu() = 32 -> L2 (32 -> 32, output-bucketed) -> crelu
//!   -> L3 (32 -> 1, output-bucketed)
//!
//! Quantisation (the engine in src/nnue.c depends on exactly this, keep in sync):
//!   l0/w, l0/b : i16, x Q0 (255)
//!   l1/w       : i8,  x Q1 / FT_SHIFT_SCALE^2, row-major [bucket*16 + out][768]
//!   l1/b       : i32, x Q * 256 (= 2^14)
//!   l2/w, l3/w : i32, x Q (64), row-major [bucket*out_size + out][in]
//!   l2/b       : i32, x Q^3
//!   l3/b       : i32, x Q^4
//!   file padded with "bullet" to a multiple of 64 bytes
//! In the engine the pairwise product is (a * b + 4) >> 3 (i16, 0..8128) and L1 runs at scale 2^20.

use bullet::{
    game::{
        formats::bulletformat::ChessBoard,
        inputs::{ChessBucketsMirrored, SparseInputType, get_num_buckets},
        outputs::{MaterialCount, OutputBuckets},
    },
    trainer::schedule::{
        lr::{self, LrScheduler},
        wdl::{self, WdlScheduler},
    },
    value::save::save_to_checkpoint,
};
use bullet_trainer::{
    model::{
        DenseInput, ModelDefinition, ModelEvaluator, ModelInputs, ModelInputsMapper, ModelWeights, SavedFormat,
        SparseInput,
    },
    optimiser::{
        Optimiser,
        adam::{AdamW, AdamWParams},
    },
    reader::ReadMapLoader,
    run::{DefaultDevice, HostPool, TrainingSchedule, TrainingSteps, train},
};
use std::env;

mod mixed_loader;
mod viri_filter;
use mixed_loader::MixedLoader;
use viri_filter::{VIRI_BUFFER_MB, VIRI_THREADS, should_keep};

// Must match NNUE_SCALE in src/nnue.h
const EVAL_SCALE: f32 = 295.0;

const L1: usize = 768;
const L2: usize = 16;
const L3: usize = 32;
const Q0: i16 = 255;
const Q1: i16 = 128;
const Q: i16 = 64;
const OUTPUT_BUCKETS: usize = 8;
const INPUT_BUCKETS: usize = get_num_buckets(&BUCKET_LAYOUT);

const FT_SHIFT: usize = 8;
const FT_SHIFT_SCALE: f32 = Q0 as f32 / ((1 << FT_SHIFT) as f32);
const I8_RANGE: f32 = i8::MAX as f32 / (Q1 as f32);
const L1_RANGE: f32 = I8_RANGE * FT_SHIFT_SCALE * FT_SHIFT_SCALE;

// Must match NNUE_INPUT_BUCKET_MAP for NNUE_ARCH=deep in src/nnue.c
#[rustfmt::skip]
const BUCKET_LAYOUT: [usize; 32] = [
     0,  1,  2,  3,
     4,  5,  6,  7,
     8,  8,  9,  9,
    10, 10, 11, 11,
    12, 12, 13, 13,
    12, 12, 13, 13,
    14, 14, 15, 15,
    14, 14, 15, 15,
];

// stage 0: warmup to peak LR and back down
const STAGE0_LR_START: f32 = 1e-4;
const STAGE0_LR_PEAK: f32 = 5e-3;
const STAGE0_WDL: f32 = 0.2;
// stage 1: main training, linear decay
const STAGE1_LR_START: f32 = 1e-3;
const STAGE1_LR_END: f32 = 1e-6;
const STAGE1_WDL_START: f32 = 0.2;
const STAGE1_WDL_END: f32 = 0.5;
// stage 2: finetune with tiny LR
const STAGE2_LR_START: f32 = 1e-5;
const STAGE2_LR_END: f32 = 1e-7;
const STAGE2_WDL: f32 = 1.0;

type InputTy = (((SparseInput, SparseInput), SparseInput), DenseInput<f32>);

fn make_mapper(inputs: &ModelInputs<InputTy>, wdl: impl WdlScheduler) -> ModelInputsMapper<ChessBoard> {
    let psqt = ChessBucketsMirrored::new(BUCKET_LAYOUT);
    let output_buckets = MaterialCount::<OUTPUT_BUCKETS>;

    ModelInputsMapper::build(inputs, move |pos: &ChessBoard, step, (((stm, ntm), bucket), target)| {
        let mut cnt = 0;
        psqt.map_features(pos, |s, n| {
            stm[cnt] = s.try_into().unwrap();
            ntm[cnt] = n.try_into().unwrap();
            cnt += 1;
        });
        if cnt < psqt.max_active() {
            stm[cnt] = -1;
            ntm[cnt] = -1;
        }

        bucket[0] = i32::from(output_buckets.bucket(pos));

        let result = f32::from(pos.result) / 2.0;
        let score = 1.0 / (1.0 + (f32::from(-pos.score) / EVAL_SCALE).exp());
        let lambda = wdl.blend(step.batch(), step.superbatch(), step.final_superbatch());
        target[0] = lambda * result + (1.0 - lambda) * score;
    })
}

fn main() {
    let args: Vec<String> = env::args().collect();

    let mut bullet_paths: Vec<String> = Vec::new();
    let mut viri_paths: Vec<String> = Vec::new();
    let mut viri_fraction: Option<f64> = None;
    let mut superbatches: usize = 1100;
    let mut start_stage: usize = 0;
    let mut start_superbatch: usize = 1;
    let mut resume: Option<String> = None;
    let mut net_id = "sleepmind_deep".to_string();
    let mut threads: u8 = 6;
    let mut save_rate: usize = 10;
    let mut eval_fens: Option<String> = None;

    let mut i = 1;
    while i < args.len() {
        let next = |i: usize| args.get(i + 1).cloned().unwrap_or_default();
        match args[i].as_str() {
            "--data" | "-d" => bullet_paths.push(next(i)),
            "--viri" => viri_paths.push(next(i)),
            "--viri-fraction" => {
                viri_fraction = Some(next(i).parse().expect("--viri-fraction must be a number in [0, 1]"))
            }
            "--superbatches" | "-s" => superbatches = next(i).parse().expect("invalid -s"),
            "--start-stage" => start_stage = next(i).parse().expect("invalid --start-stage"),
            "--start" => start_superbatch = next(i).parse().expect("invalid --start"),
            "--resume" | "-r" => resume = Some(next(i)),
            "--name" | "-n" => net_id = next(i),
            "--threads" | "-t" => threads = next(i).parse().expect("invalid -t"),
            "--save-rate" => save_rate = next(i).parse().expect("invalid --save-rate"),
            "--eval" => eval_fens = Some(next(i)),
            "--help" | "-h" => {
                println!("SleepMind deep NNUE trainer (engine: make NNUE_ARCH=deep)");
                println!();
                println!("  -d, --data <PATH>        bulletformat data file, repeatable");
                println!("      --viri <PATH>        viriformat binpack (whole games), repeatable; filtered in the loader");
                println!("      --viri-fraction <F>  share of viriformat positions per batch when mixing with --data");
                println!("                           (default: proportional to the number of positions)");
                println!("  -s, --superbatches <N>   Total superbatches over all 3 stages (default: 1100)");
                println!("                           split ~1/11 warmup, ~8/11 main, ~2/11 finetune");
                println!("      --start-stage <N>    Stage to start in: 0, 1 or 2 (default: 0)");
                println!("      --start <N>          Start superbatch within the start stage (default: 1)");
                println!("  -r, --resume <DIR>       Load full optimiser state, e.g.");
                println!("                           checkpoints/<name>-stage1-80/optimiser_state");
                println!("  -n, --name <NAME>        Network ID (default: sleepmind_deep)");
                println!("  -t, --threads <N>        Data mapping threads (default: 6)");
                println!("      --save-rate <N>      Checkpoint every N superbatches (default: 10)");
                println!("      --eval <FENFILE>     With -r: print float reference evals (cp, white view) and exit");
                return;
            }
            other => panic!("unknown argument: {other}"),
        }
        i += 2;
    }
    assert!(start_stage <= 2, "--start-stage must be 0, 1 or 2");
    if bullet_paths.is_empty() && viri_paths.is_empty() && eval_fens.is_none() {
        bullet_paths.push("data/baseline.data".to_string());
    }

    let stage0_sbs = (superbatches / 11).max(2);
    let stage2_sbs = (superbatches * 2 / 11).max(1);
    let stage1_sbs = superbatches.saturating_sub(stage0_sbs + stage2_sbs).max(1);
    let warmup_sbs = stage0_sbs / 2;

    println!("=== SleepMind deep NNUE Trainer ===");
    for path in &bullet_paths {
        println!("Dataset:   {path} (bulletformat)");
    }
    for path in &viri_paths {
        println!("Dataset:   {path} (viriformat)");
    }
    println!("Network:   {net_id}  (768x{INPUT_BUCKETS} -> {L1} pairwise -> {L2} -> {L3} -> 1, {OUTPUT_BUCKETS} out buckets)");
    println!("Stages:    0: {stage0_sbs} SBs (warmup), 1: {stage1_sbs} SBs (main), 2: {stage2_sbs} SBs (finetune)");
    println!("Start:     stage {start_stage}, superbatch {start_superbatch}");
    if let Some(r) = &resume {
        println!("Resume:    {r}");
    }

    let psqt = ChessBucketsMirrored::new(BUCKET_LAYOUT);
    let inputs = ModelInputs::default()
        .add_sparse("stm", (psqt.num_inputs(), 1), psqt.max_active())
        .add_sparse("ntm", (psqt.num_inputs(), 1), psqt.max_active())
        .add_sparse("buckets", (OUTPUT_BUCKETS, 1), 1)
        .add_dense("targets", (1, 1));

    let defn = ModelDefinition::build(&inputs, |builder, (((stm, ntm), buckets), target)| {
        // feature transformer + factoriser shared by all king buckets
        let l0f = builder.new_weights("l0/f", (L1, 768), bullet_trainer::model::InitSettings::Zeroed);
        let l0 = builder.new_affine("l0/", psqt.num_inputs(), L1);
        l0.init_with_effective_input_size(32);
        let l0w = l0.weights + l0f.repeat(INPUT_BUCKETS);
        let l0b = l0.bias;

        let l1 = builder.new_affine("l1/", L1, OUTPUT_BUCKETS * L2);
        let l2 = builder.new_affine("l2/", L2 * 2, OUTPUT_BUCKETS * L3);
        let l3 = builder.new_affine("l3/", L3, OUTPUT_BUCKETS);

        let ft = |inp, start, end| (l0w.slice_rows(start, end).matmul(inp) + l0b.slice_rows(start, end)).crelu();
        let stm_hidden = ft(stm, 0, L1 / 2) * ft(stm, L1 / 2, L1);
        let ntm_hidden = ft(ntm, 0, L1 / 2) * ft(ntm, L1 / 2, L1);
        let l0_out = stm_hidden.concat(ntm_hidden);

        // pushes FT activations towards sparsity
        let l0_out_norm = l0_out.reduce_sum_rows() / (L1 as f32);

        let l1_out = l1.forward(l0_out).select(buckets);
        let hl2 = l1_out.concat(l1_out.abs_pow(2.0)).crelu();

        let l2_out = l2.forward(hl2).select(buckets);
        let hl3 = l2_out.crelu();

        let l3_out = l3.forward(hl3).select(buckets);

        let loss = l3_out.sigmoid().squared_error(target) + 0.005 * l0_out_norm;

        (Some(loss.reduce_sum_batch()), vec![("output".to_string(), l3_out)])
    });

    let weights = ModelWeights::new(&defn, 12412421);
    let device = DefaultDevice::new(0).unwrap();
    let mut evaluator = ModelEvaluator::new(&defn, device.clone()).unwrap();
    let mut optimiser =
        Optimiser::<_, AdamW<_>>::new(defn, weights, device.clone(), AdamWParams::default()).unwrap();

    let l0_clip = AdamWParams { max_weight: 0.99, min_weight: -0.99, ..Default::default() };
    optimiser.set_params_for_weight("l0/w", l0_clip);
    optimiser.set_params_for_weight("l0/f", l0_clip);
    let l1_clip = AdamWParams { max_weight: L1_RANGE, min_weight: -L1_RANGE, ..Default::default() };
    optimiser.set_params_for_weight("l1/w", l1_clip);

    if let Some(path) = &resume {
        optimiser.load_from_checkpoint(path).expect("Failed to load checkpoint");
    }

    // Reference evaluation with bullet's float graph, used to verify the engine's integer inference
    if let Some(fen_file) = &eval_fens {
        assert!(resume.is_some(), "--eval needs -r <checkpoint>/optimiser_state");
        evaluator.load_device_weights(optimiser.weights()).unwrap();
        let mapper = make_mapper(&inputs, wdl::ConstantWDL { value: 0.0 });
        let pool = HostPool::new(device.clone());
        for fen in std::fs::read_to_string(fen_file).unwrap().lines().map(str::trim).filter(|l| !l.is_empty()) {
            let pos: ChessBoard = format!("{fen} | 0 | 0.0").parse().unwrap();
            let batch = mapper.map(&pool, &[pos], Default::default(), 1).unwrap().to_device(&device).unwrap();
            let output = evaluator.evaluate(&batch).unwrap().get("output").unwrap();
            let [value] = output.to_host().unwrap().f32()[..] else { panic!() };
            // network output is from side-to-move view, print from white's view like the engine's "eval"
            let stm_white = fen.split_whitespace().nth(1) == Some("w");
            let cp = EVAL_SCALE * value;
            println!("{:.1}\t{fen}", if stm_white { cp } else { -cp });
        }
        return;
    }

    let saved_format = vec![
        SavedFormat::id("l0/w")
            .transform(|weights, values| {
                let fac = weights.get("l0/f").values.f32().repeat(INPUT_BUCKETS);
                assert_eq!(values.len(), fac.len());
                values.iter().zip(fac).map(|(&a, b)| a + b).collect()
            })
            .round()
            .quantise::<i16>(Q0),
        SavedFormat::id("l0/b").round().quantise::<i16>(Q0),
        SavedFormat::id("l1/w")
            .transpose()
            .transform(|_, values| values.iter().map(|f| f / (FT_SHIFT_SCALE * FT_SHIFT_SCALE)).collect())
            .round()
            .quantise::<i8>(Q1),
        SavedFormat::id("l1/b").round().quantise::<i32>(i32::from(Q) * 256),
        SavedFormat::id("l2/w").transpose().round().quantise::<i32>(i32::from(Q)),
        SavedFormat::id("l2/b").round().quantise::<i32>(i32::from(Q).pow(3)),
        SavedFormat::id("l3/w").transpose().round().quantise::<i32>(i32::from(Q)),
        SavedFormat::id("l3/b").round().quantise::<i32>(i32::from(Q).pow(4)),
    ];

    let reader = MixedLoader::new(&bullet_paths, &viri_paths, should_keep, VIRI_BUFFER_MB, VIRI_THREADS, viri_fraction);

    let mut run = |stage: usize, end_superbatch: usize, lr_schedule, mapper| {
        let start = if stage == start_stage { start_superbatch } else { 1 };
        train(
            &mut optimiser,
            TrainingSchedule {
                steps: TrainingSteps {
                    batch_size: 16_384 * 8,
                    batches_per_superbatch: 6104 / 8,
                    start_superbatch: start,
                    end_superbatch,
                },
                lr_schedule,
                log_rate: 128,
                track_grad_norms: false,
            },
            ReadMapLoader::new(reader.clone(), mapper, threads),
            |_, _, _| {},
            |optimiser, step| {
                let superbatch = step.superbatch();
                if superbatch.is_multiple_of(save_rate) || superbatch == step.final_superbatch() {
                    let name = format!("{net_id}-stage{stage}-{superbatch}");
                    save_to_checkpoint(optimiser, &saved_format, &format!("checkpoints/{name}"));
                    println!("Saved [{name}]");
                }
            },
        )
        .unwrap();
    };

    std::fs::create_dir_all("checkpoints").unwrap();

    if start_stage == 0 {
        run(
            0,
            stage0_sbs,
            lr::Sequence {
                first: lr::LinearDecayLR {
                    initial_lr: STAGE0_LR_START,
                    final_lr: STAGE0_LR_PEAK,
                    final_superbatch: warmup_sbs,
                },
                second: lr::LinearDecayLR {
                    initial_lr: STAGE0_LR_PEAK,
                    final_lr: STAGE0_LR_START,
                    final_superbatch: stage0_sbs - warmup_sbs,
                },
                first_scheduler_final_superbatch: warmup_sbs,
            }
            .boxed(),
            make_mapper(&inputs, wdl::ConstantWDL { value: STAGE0_WDL }),
        );
    }

    if start_stage <= 1 {
        run(
            1,
            stage1_sbs,
            lr::LinearDecayLR { initial_lr: STAGE1_LR_START, final_lr: STAGE1_LR_END, final_superbatch: stage1_sbs }
                .boxed(),
            make_mapper(&inputs, wdl::LinearWDL { start: STAGE1_WDL_START, end: STAGE1_WDL_END }),
        );
    }

    run(
        2,
        stage2_sbs,
        lr::LinearDecayLR { initial_lr: STAGE2_LR_START, final_lr: STAGE2_LR_END, final_superbatch: stage2_sbs }
            .boxed(),
        make_mapper(&inputs, wdl::ConstantWDL { value: STAGE2_WDL }),
    );
}
