use bullet::{
    game::{
        inputs::{ChessBucketsMirrored, get_num_buckets},
        outputs::MaterialCount,
    },
    nn::{
        InitSettings, Shape,
        optimiser::{AdamW, AdamWParams},
    },
    trainer::{
        save::SavedFormat,
        schedule::{TrainingSchedule, TrainingSteps, lr, wdl},
        settings::LocalSettings,
    },
    value::ValueTrainerBuilder,
};
use std::env;

mod mixed_loader;
mod viri_filter;
use mixed_loader::MixedLoader;
use viri_filter::{VIRI_BUFFER_MB, VIRI_THREADS, should_keep};

fn main() {
    // Parse command line arguments
    let args: Vec<String> = env::args().collect();
    
    // Defaults
    let mut bullet_paths: Vec<String> = Vec::new();
    let mut viri_paths: Vec<String> = Vec::new();
    let mut viri_fraction: Option<f64> = None;
    let mut superbatches: usize = 640;
    let mut start_superbatch: usize = 1;
    let mut load_weights: Option<String> = None;
    let mut net_id = "sleepmind".to_string();
    let mut threads: usize = 2;
    let mut save_rate: usize = 10;
    let mut start_stage: usize = 0;
    let mut hl_size: usize = 768;
    let mut flat_wdl: Option<f32> = None;
    let mut batch_size: usize = 16_384 * 8;

    // Parse arguments
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--data" | "-d" => {
                i += 1;
                if i < args.len() {
                    bullet_paths.push(args[i].clone());
                }
            }
            "--viri" => {
                i += 1;
                if i < args.len() {
                    viri_paths.push(args[i].clone());
                }
            }
            "--viri-fraction" => {
                i += 1;
                if i < args.len() {
                    viri_fraction = Some(args[i].parse().expect("--viri-fraction must be a number in [0, 1]"));
                }
            }
            "--superbatches" | "-s" => {
                i += 1;
                if i < args.len() {
                    superbatches = args[i].parse().unwrap_or(640);
                }
            }
            "--start" => {
                i += 1;
                if i < args.len() {
                    start_superbatch = args[i].parse().unwrap_or(1);
                }
            }
            "--load" | "-l" => {
                i += 1;
                if i < args.len() {
                    load_weights = Some(args[i].clone());
                }
            }
            "--name" | "-n" => {
                i += 1;
                if i < args.len() {
                    net_id = args[i].clone();
                }
            }
            "--threads" | "-t" => {
                i += 1;
                if i < args.len() {
                    threads = args[i].parse().unwrap_or(2);
                }
            }
            "--save-rate" => {
                i += 1;
                if i < args.len() {
                    save_rate = args[i].parse().unwrap_or(10);
                }
            }
            "--hidden" => {
                i += 1;
                hl_size = args.get(i).and_then(|a| a.parse().ok()).expect("--hidden needs a number");
            }
            "--batch-size" => {
                i += 1;
                batch_size = args.get(i).and_then(|a| a.parse().ok()).expect("--batch-size needs a number");
            }
            "--flat-wdl" => {
                i += 1;
                flat_wdl =
                    Some(args.get(i).and_then(|a| a.parse().ok()).expect("--flat-wdl must be a number in [0, 1]"));
            }
            "--start-stage" => {
                i += 1;
                if i < args.len() {
                    start_stage = args[i].parse().unwrap_or(0);
                }
            }
            "--help" | "-h" => {
                println!("SleepMind NNUE Trainer");
                println!();
                println!("Usage: training [OPTIONS]");
                println!();
                println!("Options:");
                println!("  -d, --data <PATH>        bulletformat data file, repeatable (default: data/baseline.data");
                println!("                           if neither --data nor --viri is given)");
                println!("      --viri <PATH>        viriformat binpack (whole games), repeatable; filtered in the loader");
                println!("      --viri-fraction <F>  share of viriformat positions per batch when mixing with --data");
                println!("                           (default: proportional to the number of positions per epoch)");
                println!("  -s, --superbatches <N>   Total superbatches over all 3 stages (default: 640)");
                println!("                           split ~1/11 warmup, ~8/11 main, ~2/11 finetune");
                println!("      --start-stage <N>    Stage to start in: 0, 1 or 2 (default: 0, use for resuming)");
                println!("      --start <N>          Start superbatch within the start stage (default: 1)");
                println!("  -l, --load <PATH>        Load weights from file (.wgts)");
                println!("  -n, --name <NAME>        Network ID for output (default: sleepmind)");
                println!("  -t, --threads <N>        Number of threads (default: 2)");
                println!("      --hidden <N>         Hidden layer size per perspective (default: 768); the engine needs");
                println!("                           the matching build, e.g. make NNUE_ARCH=legacy hl_64=1");
                println!("      --batch-size <N>     Positions per batch (default: 131072); a superbatch always has");
                println!("                           ~100 M positions, so smaller batches mean more optimiser steps");
                println!("      --flat-wdl <W>       Single stage instead of the 3 stages: all superbatches with constant");
                println!("                           WDL share W and cosine LR decay, no warmup, no finetune");
                println!("      --save-rate <N>      Save checkpoint every N superbatches (default: 10)");
                println!("  -h, --help               Show this help");
                println!();
                println!("Examples:");
                println!("  # First training run with 10 superbatches");
                println!("  training -d data/hce_games.data -s 10 -n sleepmind_v1");
                println!();
                println!("  # Continue training from checkpoint");
                println!("  training -d data/more_games.data -s 50 --start 11 -l checkpoints/sleepmind_v1-10.wgts -n sleepmind_v1");
                println!();
                println!("  # Resume in stage 1 at superbatch 81");
                println!("  training -s 640 --start-stage 1 --start 81 -l checkpoints/sleepmind_v1-stage1-80/optimiser_state/weights.bin -n sleepmind_v1");
                return;
            }
            other => {
                eprintln!("unknown argument: {other} (see --help)");
                std::process::exit(1);
            }
        }
        i += 1;
    }
    
    if bullet_paths.is_empty() && viri_paths.is_empty() {
        bullet_paths.push("data/baseline.data".to_string());
    }

    println!("=== SleepMind NNUE Trainer ===");
    for path in &bullet_paths {
        println!("Dataset:       {path} (bulletformat)");
    }
    for path in &viri_paths {
        println!("Dataset:       {path} (viriformat)");
    }
    println!("Superbatches:  {} (starting from {})", superbatches, start_superbatch);
    println!("Network ID:    {}", net_id);
    println!("Threads:       {}", threads);
    if let Some(ref path) = load_weights {
        println!("Loading weights: {}", path);
    }
    println!();

    // hyperparams
    println!("Hidden size:   {hl_size}");
    // --flat-wdl: final LR of the cosine decay (bullet's usual 0.001 * 0.3^5)
    const FLAT_LR_END: f32 = 2.43e-6;
    // stage 0: warmup to peak LR and back down
    const STAGE0_LR_START: f32 = 1e-4;
    const STAGE0_LR_PEAK: f32 = 5e-3;
    const STAGE0_WDL: f32 = 0.2;
    // stage 1: main training, linear decay (as in the bullet advanced example)
    const STAGE1_LR_START: f32 = 1e-3;
    const STAGE1_LR_END: f32 = 1e-6;
    const STAGE1_WDL_START: f32 = 0.2;
    const STAGE1_WDL_END: f32 = 0.5;
    // stage 2: finetune with tiny LR
    const STAGE2_LR_START: f32 = 1e-5;
    const STAGE2_LR_END: f32 = 1e-7;
    const STAGE2_WDL: f32 = 1.0;
    const EVAL_SCALE: f32 = 295.0;
    const NUM_OUTPUT_BUCKETS: usize = 8;
    #[rustfmt::skip]
    const BUCKET_LAYOUT: [usize; 32] = [
        0, 1, 2, 3,
        4, 4, 5, 5,
        6, 6, 6, 6,
        7, 7, 7, 7,
        8, 8, 8, 8,
        8, 8, 8, 8,
        9, 9, 9, 9,
        9, 9, 9, 9,
    ];

    const NUM_INPUT_BUCKETS: usize = get_num_buckets(&BUCKET_LAYOUT);

    let mut trainer = ValueTrainerBuilder::default()
        .dual_perspective()
        .optimiser(AdamW)
        .inputs(ChessBucketsMirrored::new(BUCKET_LAYOUT))
        .output_buckets(MaterialCount::<NUM_OUTPUT_BUCKETS>)
        .save_format(&[
            // merge in the factoriser weights
            SavedFormat::id("l0w")
                .transform(|store, weights| {
                    let factoriser = store.get("l0f").values.f32().repeat(NUM_INPUT_BUCKETS);
                    weights.into_iter().zip(factoriser).map(|(a, b)| a + b).collect()
                })
                .round()
                .quantise::<i16>(255),
            SavedFormat::id("l0b").round().quantise::<i16>(255),
            SavedFormat::id("l1w").round().quantise::<i16>(64).transpose(),
            SavedFormat::id("l1b").round().quantise::<i16>(255 * 64),
        ])
        .loss_fn(|output, target| output.sigmoid().squared_error(target))
        .build(|builder, stm_inputs, ntm_inputs, output_buckets| {
            // input layer factoriser
            let l0f = builder.new_weights("l0f", Shape::new(hl_size, 768), InitSettings::Zeroed);
            let expanded_factoriser = l0f.repeat(NUM_INPUT_BUCKETS);

            // input layer weights
            let mut l0 = builder.new_affine("l0", 768 * NUM_INPUT_BUCKETS, hl_size);
            l0.weights = l0.weights + expanded_factoriser;

            // output layer weights
            let l1 = builder.new_affine("l1", 2 * hl_size, NUM_OUTPUT_BUCKETS);

            // inference
            let stm_hidden = l0.forward(stm_inputs).screlu();
            let ntm_hidden = l0.forward(ntm_inputs).screlu();
            let hidden_layer = stm_hidden.concat(ntm_hidden);
            l1.forward(hidden_layer).select(output_buckets)
        });

    // Load weights if specified
    if let Some(ref path) = load_weights {
        println!("Loading weights from: {}", path);
        trainer.optimiser.load_weights_from_file(path).expect("Failed to load weights");
    }

    // need to account for factoriser weight magnitudes
    let stricter_clipping = AdamWParams { max_weight: 0.99, min_weight: -0.99, ..Default::default() };
    trainer.optimiser.set_params_for_weight("l0w", stricter_clipping);
    trainer.optimiser.set_params_for_weight("l0f", stricter_clipping);

    // 317690799

    // Stage lengths, split like the bullet advanced example (100 / 800 / 200)
    let stage0_sbs = (superbatches / 11).max(2);
    let stage2_sbs = (superbatches * 2 / 11).max(1);
    let stage1_sbs = superbatches.saturating_sub(stage0_sbs + stage2_sbs).max(1);
    let warmup_sbs = stage0_sbs / 2;

    println!("Stages:        0: {stage0_sbs} SBs (warmup), 1: {stage1_sbs} SBs (main), 2: {stage2_sbs} SBs (finetune)");
    assert!(start_stage <= 2, "--start-stage must be 0, 1 or 2");

    let settings = LocalSettings { threads, test_set: None, output_directory: "checkpoints", batch_queue_size: 32 };

    let dataloader =
        MixedLoader::new(&bullet_paths, &viri_paths, should_keep, VIRI_BUFFER_MB, VIRI_THREADS, viri_fraction);

    // --start only applies to the stage we resume in, later stages start at 1
    let steps = |stage: usize, end_superbatch: usize| TrainingSteps {
        batch_size,
        batches_per_superbatch: 6104 * 16_384 / batch_size,
        start_superbatch: if stage == start_stage { start_superbatch } else { 1 },
        end_superbatch,
    };

    // Single flat stage (short data comparison runs): constant WDL, cosine decay from the
    // main-stage start LR, checkpoints named <name>-stage0-<superbatch>.
    if let Some(value) = flat_wdl {
        assert!(start_stage == 0, "--flat-wdl has only stage 0");
        println!("Flat run:      {superbatches} SBs, WDL {value}, LR {STAGE1_LR_START} -> {FLAT_LR_END} (cosine)");
        let schedule = TrainingSchedule {
            net_id: format!("{net_id}-stage0"),
            eval_scale: EVAL_SCALE,
            steps: steps(0, superbatches),
            wdl_scheduler: wdl::ConstantWDL { value },
            lr_scheduler: lr::CosineDecayLR {
                initial_lr: STAGE1_LR_START,
                final_lr: FLAT_LR_END,
                final_superbatch: superbatches,
            },
            save_rate,
        };
        trainer.run(&schedule, &settings, &dataloader);
        return;
    }

    // Stage 0: LR warmup + cooldown, mostly eval targets
    if start_stage <= 0 {
        let schedule = TrainingSchedule {
            net_id: format!("{net_id}-stage0"),
            eval_scale: EVAL_SCALE,
            steps: steps(0, stage0_sbs),
            wdl_scheduler: wdl::ConstantWDL { value: STAGE0_WDL },
            lr_scheduler: lr::Sequence {
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
            },
            save_rate,
        };
        trainer.run(&schedule, &settings, &dataloader);
    }

    // Stage 1: main training, WDL ramps up linearly
    if start_stage <= 1 {
        let schedule = TrainingSchedule {
            net_id: format!("{net_id}-stage1"),
            eval_scale: EVAL_SCALE,
            steps: steps(1, stage1_sbs),
            wdl_scheduler: wdl::LinearWDL { start: STAGE1_WDL_START, end: STAGE1_WDL_END },
            lr_scheduler: lr::LinearDecayLR {
                initial_lr: STAGE1_LR_START,
                final_lr: STAGE1_LR_END,
                final_superbatch: stage1_sbs,
            },
            save_rate,
        };
        trainer.run(&schedule, &settings, &dataloader);
    }

    // Stage 2: finetune with tiny LR on (mostly) game results
    let schedule = TrainingSchedule {
        net_id: format!("{net_id}-stage2"),
        eval_scale: EVAL_SCALE,
        steps: steps(2, stage2_sbs),
        wdl_scheduler: wdl::ConstantWDL { value: STAGE2_WDL },
        lr_scheduler: lr::LinearDecayLR {
            initial_lr: STAGE2_LR_START,
            final_lr: STAGE2_LR_END,
            final_superbatch: stage2_sbs,
        },
        save_rate,
    };
    trainer.run(&schedule, &settings, &dataloader);
}
