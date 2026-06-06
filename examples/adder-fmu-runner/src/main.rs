//! # adder-fmu-runner
//!
//! Host runner that loads the `adder-fmu` WebAssembly component via wasmtime
//! and drives a Co-Simulation from t = 0.0 s to t = 5.0 s in 0.1 s steps.
//!
//! ## Inputs
//!
//! | Variable  | VR | Initial | Slope (per second) |
//! |-----------|----|---------|--------------------|
//! | `input_a` |  0 |   0.0   |        1.0         |
//! | `input_b` |  1 |   2.0   |        2.0         |
//!
//! At communication point `t` the expected values are:
//!   - `input_a(t) = t`
//!   - `input_b(t) = 2.0 + 2.0 * t`
//!   - `sum(t)     = input_a(t) + input_b(t)`
//!
//! ## WIT limitation workaround
//!
//! The `co-simulation-fmu` WIT world's `from-instance` function consumes the
//! `common::instance` handle (owned transfer), so the host can no longer call
//! `get-float64` / `set-float64` on the same handle after conversion.  Because
//! the adder FMU is a purely algebraic, memoryless component, we work around
//! this by instantiating the FMU fresh at each communication point:
//!
//! 1. Set inputs at time `t` on a fresh `instance`.
//! 2. Read and assert the output via `get-float64`.
//! 3. Wrap it with `from-instance` → `co-simulation-instance`.
//! 4. Call `do-step` to exercise the co-simulation stepping mechanism.
//! 5. Drop the `co-simulation-instance`; repeat.
//!
//! ## Usage
//!
//! ```text
//! adder-fmu-runner <path/to/adder_fmu.wasm>
//! ```

use anyhow::{anyhow, bail, Context, Result};
use wasmtime::component::{Component, Linker, ResourceAny};
use wasmtime::{Config, Engine, Store};
use wasmtime_wasi::{WasiCtx, WasiCtxBuilder, WasiCtxView, WasiView};

// ---------------------------------------------------------------------------
// WIT bindings (host side)
// ---------------------------------------------------------------------------
wasmtime::component::bindgen!({
    world: "co-simulation-fmu",
    path:  "../../wit",
});

use fmi::fmi3::types::Status;

// ---------------------------------------------------------------------------
// Host state
// ---------------------------------------------------------------------------

/// Combined state for the wasmtime store.
///
/// Must implement [`WasiView`] (for WASI P2 imports from the Rust std-lib
/// embedded in the component) and the FMI callback host traits.
struct HostState {
    wasi:  WasiCtx,
    table: wasmtime::component::ResourceTable,
}

impl WasiView for HostState {
    fn ctx(&mut self) -> WasiCtxView<'_> {
        WasiCtxView {
            ctx: &mut self.wasi,
            table: &mut self.table,
        }
    }
}

/// Host implementation of `fmi:fmi3/callbacks`.
impl fmi::fmi3::callbacks::Host for HostState {
    fn log_message(
        &mut self,
        instance_name: String,
        status:        Status,
        category:      String,
        message:       String,
    ) {
        eprintln!("[{status:?}] {instance_name} ({category}): {message}");
    }

    fn clock_update(&mut self) {}

    fn lock_preemption(&mut self) {}

    fn unlock_preemption(&mut self) {}
}

/// Host implementation of `fmi:fmi3/intermediate-update-callbacks`.
///
/// The adder FMU does not use early-return mode, so this is a no-op.
impl fmi::fmi3::intermediate_update_callbacks::Host for HostState {
    fn intermediate_update(
        &mut self,
        _intermediate_update_time:            f64,
        _intermediate_variable_set_requested: bool,
        _intermediate_variable_get_allowed:   bool,
        _intermediate_step_finished:          bool,
        _can_return_early:                    bool,
    ) -> (bool, f64) {
        (false, 0.0)
    }
}

/// Host implementation for `fmi:fmi3/types`.
///
/// This interface only contains type definitions, so no methods are required.
impl fmi::fmi3::types::Host for HostState {}

// ---------------------------------------------------------------------------
// Value-reference constants (must match the adder-fmu implementation)
// ---------------------------------------------------------------------------
const VR_INPUT_A: u32 = 0;
const VR_INPUT_B: u32 = 1;
const VR_SUM:     u32 = 2;

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

fn main() -> Result<()> {
    let wasm_path = std::env::args()
        .nth(1)
        .context("Usage: adder-fmu-runner <path/to/adder_fmu.wasm>")?;

    // ── Engine & component ─────────────────────────────────────────────────
    let mut cfg = Config::new();
    cfg.wasm_component_model(true);
    let engine = Engine::new(&cfg)?;

    let component = Component::from_file(&engine, &wasm_path)
        .map_err(|e| anyhow!("Failed to load component from '{wasm_path}': {e}"))?;

    // ── Linker: WASI P2 + FMI callbacks ───────────────────────────────────
    let mut linker: Linker<HostState> = Linker::new(&engine);
    wasmtime_wasi::p2::add_to_linker_sync(&mut linker)?;
    CoSimulationFmu::add_to_linker::<HostState, wasmtime::component::HasSelf<HostState>>(
        &mut linker,
        |s: &mut HostState| s,
    )?;

    // ── Store ──────────────────────────────────────────────────────────────
    let mut store = Store::new(
        &engine,
        HostState {
            wasi:  WasiCtxBuilder::new().build(),
            table: wasmtime::component::ResourceTable::new(),
        },
    );

    // ── Instantiate the component ──────────────────────────────────────────
    let world = CoSimulationFmu::instantiate(&mut store, &component, &linker)
        .map_err(|e| anyhow!("Failed to instantiate the co-simulation FMU component: {e}"))?;

    // ── Simulation parameters ──────────────────────────────────────────────
    let step_size:   f64   = 0.1;
    let n_steps:     usize = 50;          // 0 … 5.0 s
    let tolerance:   f64   = 1e-10;       // floating-point comparison threshold

    println!(
        "Running adder-fmu from t=0.0 to t={:.1} s in {step_size} s steps …",
        n_steps as f64 * step_size
    );

    // ── Co-simulation loop ─────────────────────────────────────────────────
    //
    // At each communication point `t = i * step_size`:
    //
    //   input_a(t) = t               (start 0, slope 1 per second)
    //   input_b(t) = 2.0 + 2.0 * t  (start 2, slope 2 per second)
    //   sum(t)     = input_a + input_b  (expected FMU output)
    //
    // A fresh FMU instance is created at every step so that `set-float64`
    // and `get-float64` (both on `common::instance`) can be called before the
    // instance is consumed by `from-instance` (see module-level doc comment).
    for i in 0..=n_steps {
        let t            = i as f64 * step_size;
        let input_a      = t;
        let input_b      = 2.0 + 2.0 * t;
        let expected_sum = input_a + input_b;

        // -- 1. Instantiate ---------------------------------------------------
        let inst: ResourceAny = world
            .fmi_fmi3_common()
            .instance()
            .call_instantiate_co_simulation(
                &mut store,
                "adder-1",   // instance name
                "",          // instantiation token
                "",          // resource path
                false,       // visible
                false,       // logging on
                false,       // event mode used
                false,       // early return allowed
                &[],         // required intermediate variables
            )?
            .with_context(|| format!("FMU instantiation returned None at step {i}"))?;

        // -- 2. Initialization mode ------------------------------------------
        world
            .fmi_fmi3_common()
            .instance()
            .call_enter_initialization_mode(&mut store, inst, None, t, None)?;

        // -- 3. Set inputs at current time point -----------------------------
        world
            .fmi_fmi3_common()
            .instance()
            .call_set_float64(
                &mut store,
                inst,
                &[VR_INPUT_A, VR_INPUT_B],
                &[input_a, input_b],
            )?;

        // -- 4. Exit initialization mode -------------------------------------
        world
            .fmi_fmi3_common()
            .instance()
            .call_exit_initialization_mode(&mut store, inst)?;

        // -- 5. Read and verify output ---------------------------------------
        let get_result = world
            .fmi_fmi3_common()
            .instance()
            .call_get_float64(&mut store, inst, &[VR_SUM])?;

        let values = match get_result {
            Ok(v)  => v,
            Err(s) => bail!("get-float64 failed at step {i} (t={t:.1}) with status {s:?}"),
        };

        let got = values[0];
        assert!(
            (got - expected_sum).abs() < tolerance,
            "step {i} (t={t:.1}): output sum = {got} but expected {expected_sum} \
             (input_a={input_a}, input_b={input_b})",
        );

        // -- 6. Wrap into co-simulation-instance (consumes `inst`) ----------
        //
        // `from-instance` takes ownership of the `instance` handle; after this
        // call we can no longer use `inst`.
        let cs: ResourceAny = world
            .fmi_fmi3_co_simulation()
            .co_simulation_instance()
            .call_from_instance(&mut store, inst)?;

        // -- 7. Advance time by one step (not at the final point) ------------
        if i < n_steps {
            let do_step_result = world
                .fmi_fmi3_co_simulation()
                .co_simulation_instance()
                .call_do_step(&mut store, cs, t, step_size, false)?;

            let r = match do_step_result {
                Ok(r)  => r,
                Err(s) => bail!("do-step failed at step {i} (t={t:.1}) with status {s:?}"),
            };

            assert!(
                !r.terminate_simulation,
                "do-step at step {i} (t={t:.1}) unexpectedly requested termination",
            );
            assert!(
                !r.early_return,
                "do-step at step {i} (t={t:.1}) had unexpected early return",
            );
            let lst = r.last_successful_time;
            let expected_lst = t + step_size;
            assert!(
                (lst - expected_lst).abs() < tolerance,
                "do-step at step {i} (t={t:.1}): last_successful_time={lst} but expected {expected_lst}",
            );
        }

        // `cs` is dropped here, releasing the component resource handle.
    }

    println!(
        "OK — all {n_steps} steps passed: \
         adder FMU correctly computes sum = input_a + input_b at every communication point."
    );
    Ok(())
}
