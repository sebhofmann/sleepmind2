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
    value::{ValueTrainerBuilder, loader::DirectSequentialDataLoader},
};
use std::env;

fn main() {
    // Parse command line arguments
    let args: Vec<String> = env::args().collect();
    
    // Defaults
    let mut dataset_path = "data/baseline.data".to_string();
    let mut superbatches: usize = 640;
    let mut start_superbatch: usize = 1;
    let mut load_weights: Option<String> = None;
    let mut net_id = "sleepmind".to_string();
    let mut threads: usize = 2;
    let mut save_rate: usize = 10;
    
    // Parse arguments
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--data" | "-d" => {
                i += 1;
                if i < args.len() {
                    dataset_path = args[i].clone();
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
            "--help" | "-h" => {
                println!("SleepMind NNUE Trainer");
                println!();
                println!("Usage: training [OPTIONS]");
                println!();
                println!("Options:");
                println!("  -d, --data <PATH>        Training data file (default: data/baseline.data)");
                println!("  -s, --superbatches <N>   Number of superbatches (default: 640)");
                println!("      --start <N>          Start superbatch (default: 1, use for resuming)");
                println!("  -l, --load <PATH>        Load weights from file (.wgts)");
                println!("  -n, --name <NAME>        Network ID for output (default: sleepmind)");
                println!("  -t, --threads <N>        Number of threads (default: 2)");
                println!("      --save-rate <N>      Save checkpoint every N superbatches (default: 10)");
                println!("  -h, --help               Show this help");
                println!();
                println!("Examples:");
                println!("  # First training run with 10 superbatches");
                println!("  training -d data/hce_games.data -s 10 -n sleepmind_v1");
                println!();
                println!("  # Continue training from checkpoint");
                println!("  training -d data/more_games.data -s 50 --start 11 -l checkpoints/sleepmind_v1-10.wgts -n sleepmind_v1");
                return;
            }
            _ => {}
        }
        i += 1;
    }
    
    println!("=== SleepMind NNUE Trainer ===");
    println!("Dataset:       {}", dataset_path);
    println!("Superbatches:  {} (starting from {})", superbatches, start_superbatch);
    println!("Network ID:    {}", net_id);
    println!("Threads:       {}", threads);
    if let Some(ref path) = load_weights {
        println!("Loading weights: {}", path);
    }
    println!();

    // hyperparams
    let hl_size = 768;
    let initial_lr = 0.001;
    let final_lr = 0.001 * 0.3f32.powi(5);
    let wdl_proportion = 0.00;
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
                    let factoriser = store.get("l0f").values.repeat(NUM_INPUT_BUCKETS);
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

    let schedule = TrainingSchedule {
        net_id,
        eval_scale: 400.0,
        steps: TrainingSteps {
            batch_size: 16_384,
            batches_per_superbatch: 6104,
            start_superbatch,
            end_superbatch: superbatches,
        },
        wdl_scheduler: wdl::ConstantWDL { value: wdl_proportion },
        lr_scheduler: lr::CosineDecayLR { initial_lr, final_lr, final_superbatch: superbatches },
        save_rate,
    };

    let settings = LocalSettings { threads, test_set: None, output_directory: "checkpoints", batch_queue_size: 32 };

    let dataloader = DirectSequentialDataLoader::new(&[&dataset_path]);

    trainer.run(&schedule, &settings, &dataloader);
}
