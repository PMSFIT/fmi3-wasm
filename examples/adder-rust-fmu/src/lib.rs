//! # adder-rust-fmu
//!
//! A minimal FMI 3.0 Co-Simulation FMU implemented as a WebAssembly component.
//!
//! ## Variables
//!
//! | Value Reference | Name      | Causality     | Type    | Description               |
//! |-----------------|-----------|---------------|---------|---------------------------|
//! | 0               | `time`    | independent   | Float64 | Independent time variable |
//! | 1               | `input_a` | input         | Float64 | First addend              |
//! | 2               | `input_b` | input         | Float64 | Second addend             |
//! | 3               | `sum`     | output        | Float64 | sum = input_a + input_b   |
//!
//! ## Lifecycle
//!
//! ```text
//! co-simulation-instance::instantiate-co-simulation(...)  → cs
//! co-simulation-instance::enter-initialization-mode(cs, ...)
//! co-simulation-instance::set-float64(cs, [1, 2], [a0, b0])
//! co-simulation-instance::exit-initialization-mode(cs)
//! loop:
//!   co-simulation-instance::set-float64(cs, [1, 2], [a, b])
//!   co-simulation-instance::get-float64(cs, [0, 3])
//!   co-simulation-instance::do-step(cs, t, h, false)
//! co-simulation-instance::terminate(cs)
//! ```

#![allow(unused_variables)]

// ---------------------------------------------------------------------------
// WIT bindings
// ---------------------------------------------------------------------------
wit_bindgen::generate!({
    world: "co-simulation-fmu",
    path:  "../../wit",
});

use std::cell::RefCell;

use exports::fmi::fmi3::{
    co_simulation::{
        DoStepResult, Guest as CoSimulationGuest, GuestCoSimulationInstance,
    },
    common::Guest as CommonGuest,
};
use exports::fmi::fmi3::co_simulation::{
    DiscreteStatesInfo, IntervalFraction, IntervalQualifier, Status, VariableDependency,
};
use fmi::fmi3::types::DependencyKind;

// ---------------------------------------------------------------------------
// Value-reference assignments
// ---------------------------------------------------------------------------
const VR_TIME:       u32 = 0;
const VR_INPUT_A:    u32 = 1;
const VR_INPUT_B:    u32 = 2;
const VR_OUTPUT_SUM: u32 = 3;

// ---------------------------------------------------------------------------
// Shared simulation state
// ---------------------------------------------------------------------------
#[derive(Clone, Default)]
struct AdderData {
    in_initialization_mode: bool,
    current_time: f64,
    input_a: f64,
    input_b: f64,
    output_sum: f64,
}

thread_local! {
    static STORE: RefCell<Vec<AdderData>> = RefCell::new(Vec::new());
}

fn store_alloc() -> usize {
    STORE.with_borrow_mut(|s| {
        s.push(AdderData::default());
        s.len() - 1
    })
}

fn store_get<R>(idx: usize, f: impl FnOnce(&AdderData) -> R) -> R {
    STORE.with_borrow(|s| f(&s[idx]))
}

fn store_set<R>(idx: usize, f: impl FnOnce(&mut AdderData) -> R) -> R {
    STORE.with_borrow_mut(|s| f(&mut s[idx]))
}

// ---------------------------------------------------------------------------
// co-simulation::co-simulation-instance resource
// ---------------------------------------------------------------------------

pub struct AdderCoSimInstance {
    idx: usize,
}

impl GuestCoSimulationInstance for AdderCoSimInstance {

    // ── Static factory ───────────────────────────────────────────────────────

    fn instantiate_co_simulation(
        _instance_name:                   String,
        _instantiation_token:             String,
        _resource_path:                   String,
        _visible:                         bool,
        _logging_on:                      bool,
        _event_mode_used:                 bool,
        _early_return_allowed:            bool,
        _required_intermediate_variables: Vec<u32>,
    ) -> Option<exports::fmi::fmi3::co_simulation::CoSimulationInstance> {
        let idx = store_alloc();
        Some(exports::fmi::fmi3::co_simulation::CoSimulationInstance::new(
            AdderCoSimInstance { idx },
        ))
    }

    // ── Debug logging ────────────────────────────────────────────────────────

    fn set_debug_logging(&self, _logging_on: bool, _categories: Vec<String>) -> Status {
        Status::Ok
    }

    // ── Lifecycle ────────────────────────────────────────────────────────────

    fn enter_initialization_mode(
        &self,
        _tolerance:  Option<f64>,
        _start_time: f64,
        _stop_time:  Option<f64>,
    ) -> Status {
        store_set(self.idx, |d| {
            d.in_initialization_mode = true;
        });
        Status::Ok
    }

    fn exit_initialization_mode(&self) -> Status {
        store_set(self.idx, |d| {
            d.in_initialization_mode = false;
        });
        Status::Ok
    }

    fn enter_event_mode(&self) -> Status {
        Status::Error
    }

    fn update_discrete_states(&self) -> Result<DiscreteStatesInfo, Status> {
        Err(Status::Error)
    }

    fn terminate(&self) -> Status {
        Status::Ok
    }

    fn reset(&self) -> Status {
        store_set(self.idx, |d| {
            d.in_initialization_mode = false;
            d.current_time = 0.0;
            d.input_a = 0.0;
            d.input_b = 0.0;
            d.output_sum = 0.0;
        });
        Status::Ok
    }

    fn enter_configuration_mode(&self) -> Status {
        Status::Error
    }

    fn exit_configuration_mode(&self) -> Status {
        Status::Error
    }

    // ── Float64 getter / setter ──────────────────────────────────────────────

    fn get_float64(&self, value_references: Vec<u32>) -> Result<Vec<f64>, Status> {
        store_get(self.idx, |d| {
            value_references
                .iter()
                .map(|vr| match *vr {
                    VR_TIME       => Ok(d.current_time),
                    VR_INPUT_A    => Ok(d.input_a),
                    VR_INPUT_B    => Ok(d.input_b),
                    VR_OUTPUT_SUM => Ok(d.output_sum),
                    _             => Err(Status::Error),
                })
                .collect()
        })
    }

    fn set_float64(&self, value_references: Vec<u32>, values: Vec<f64>) -> Status {
        store_set(self.idx, |d| {
            for (vr, &val) in value_references.iter().zip(values.iter()) {
                match *vr {
                    VR_INPUT_A => d.input_a = val,
                    VR_INPUT_B => d.input_b = val,
                    _          => return Status::Error,
                }
            }
            Status::Ok
        });
        if store_get(self.idx, |d| d.in_initialization_mode) {
            // Update output sum immediately when in initialization mode.
            store_set(self.idx, |d| {
                d.output_sum = d.input_a + d.input_b;
            });
        }
        Status::Ok
    }

    // ── All other typed getters – not supported ──────────────────────────────

    fn get_float32(&self, _: Vec<u32>) -> Result<Vec<f32>,      Status> { Err(Status::Error) }
    fn get_int8   (&self, _: Vec<u32>) -> Result<Vec<i8>,       Status> { Err(Status::Error) }
    fn get_int16  (&self, _: Vec<u32>) -> Result<Vec<i16>,      Status> { Err(Status::Error) }
    fn get_int32  (&self, _: Vec<u32>) -> Result<Vec<i32>,      Status> { Err(Status::Error) }
    fn get_int64  (&self, _: Vec<u32>) -> Result<Vec<i64>,      Status> { Err(Status::Error) }
    fn get_uint8  (&self, _: Vec<u32>) -> Result<Vec<u8>,       Status> { Err(Status::Error) }
    fn get_uint16 (&self, _: Vec<u32>) -> Result<Vec<u16>,      Status> { Err(Status::Error) }
    fn get_uint32 (&self, _: Vec<u32>) -> Result<Vec<u32>,      Status> { Err(Status::Error) }
    fn get_uint64 (&self, _: Vec<u32>) -> Result<Vec<u64>,      Status> { Err(Status::Error) }
    fn get_boolean(&self, _: Vec<u32>) -> Result<Vec<bool>,     Status> { Err(Status::Error) }
    fn get_string (&self, _: Vec<u32>) -> Result<Vec<String>,   Status> { Err(Status::Error) }
    fn get_binary (&self, _: Vec<u32>) -> Result<Vec<Vec<u8>>,  Status> { Err(Status::Error) }
    fn get_clock  (&self, _: Vec<u32>) -> Result<Vec<bool>,     Status> { Err(Status::Error) }

    // ── All other typed setters – not supported ──────────────────────────────

    fn set_float32(&self, _: Vec<u32>, _: Vec<f32>)     -> Status { Status::Error }
    fn set_int8   (&self, _: Vec<u32>, _: Vec<i8>)      -> Status { Status::Error }
    fn set_int16  (&self, _: Vec<u32>, _: Vec<i16>)     -> Status { Status::Error }
    fn set_int32  (&self, _: Vec<u32>, _: Vec<i32>)     -> Status { Status::Error }
    fn set_int64  (&self, _: Vec<u32>, _: Vec<i64>)     -> Status { Status::Error }
    fn set_uint8  (&self, _: Vec<u32>, _: Vec<u8>)      -> Status { Status::Error }
    fn set_uint16 (&self, _: Vec<u32>, _: Vec<u16>)     -> Status { Status::Error }
    fn set_uint32 (&self, _: Vec<u32>, _: Vec<u32>)     -> Status { Status::Error }
    fn set_uint64 (&self, _: Vec<u32>, _: Vec<u64>)     -> Status { Status::Error }
    fn set_boolean(&self, _: Vec<u32>, _: Vec<bool>)    -> Status { Status::Error }
    fn set_string (&self, _: Vec<u32>, _: Vec<String>)  -> Status { Status::Error }
    fn set_binary (&self, _: Vec<u32>, _: Vec<Vec<u8>>) -> Status { Status::Error }
    fn set_clock  (&self, _: Vec<u32>, _: Vec<bool>)    -> Status { Status::Error }

    // ── Variable dependencies ────────────────────────────────────────────────

    fn get_number_of_variable_dependencies(&self, vr: u32) -> Result<u64, Status> {
        match vr {
            VR_OUTPUT_SUM => Ok(2),  // depends on VR_INPUT_A and VR_INPUT_B
            _             => Err(Status::Error),
        }
    }

    fn get_variable_dependencies(
        &self,
        dependent: u32,
    ) -> Result<Vec<VariableDependency>, Status> {
        match dependent {
            VR_OUTPUT_SUM => Ok(vec![
                VariableDependency {
                    dependent_value_reference:    VR_OUTPUT_SUM,
                    element_index_of_dependent:   0,
                    independent_value_reference:  VR_INPUT_A,
                    element_index_of_independent: 0,
                    kind: DependencyKind::Dependent,
                },
                VariableDependency {
                    dependent_value_reference:    VR_OUTPUT_SUM,
                    element_index_of_dependent:   0,
                    independent_value_reference:  VR_INPUT_B,
                    element_index_of_independent: 0,
                    kind: DependencyKind::Dependent,
                },
            ]),
            _ => Err(Status::Error),
        }
    }

    // ── FMU state serialisation ──────────────────────────────────────────────

    fn get_fmu_state(&self) -> Result<Vec<u8>, Status> {
        store_get(self.idx, |d| {
            let mut bytes = Vec::with_capacity(1 + 4 * 8);
            bytes.push(if d.in_initialization_mode { 1 } else { 0 });
            bytes.extend_from_slice(&d.current_time.to_le_bytes());
            bytes.extend_from_slice(&d.input_a.to_le_bytes());
            bytes.extend_from_slice(&d.input_b.to_le_bytes());
            bytes.extend_from_slice(&d.output_sum.to_le_bytes());
            Ok(bytes)
        })
    }

    fn set_fmu_state(&self, state: Vec<u8>) -> Status {
        if state.len() < 1 + 4 * 8 {
            return Status::Error;
        }
        let in_initialization_mode = state[0] != 0;
        let time = f64::from_le_bytes(state[1..9].try_into().unwrap());
        let a = f64::from_le_bytes(state[9..17].try_into().unwrap());
        let b = f64::from_le_bytes(state[17..25].try_into().unwrap());
        let output_sum = f64::from_le_bytes(state[25..33].try_into().unwrap());
        store_set(self.idx, |d| {
            d.in_initialization_mode = in_initialization_mode;
            d.current_time = time;
            d.input_a = a;
            d.input_b = b;
            d.output_sum = output_sum;
        });
        Status::Ok
    }

    // ── Directional / adjoint derivatives ───────────────────────────────────

    fn get_directional_derivative(
        &self,
        unknowns: Vec<u32>,
        knowns:   Vec<u32>,
        seed:     Vec<f64>,
    ) -> Result<Vec<f64>, Status> {
        if unknowns != [VR_OUTPUT_SUM] {
            return Err(Status::Error);
        }
        let sensitivity: f64 = knowns
            .iter()
            .zip(seed.iter())
            .map(|(vr, s)| match *vr {
                VR_INPUT_A | VR_INPUT_B => s,
                _                       => &0.0,
            })
            .sum();
        Ok(vec![sensitivity])
    }

    fn get_adjoint_derivative(
        &self,
        unknowns: Vec<u32>,
        knowns:   Vec<u32>,
        seed:     Vec<f64>,
    ) -> Result<Vec<f64>, Status> {
        if unknowns != [VR_OUTPUT_SUM] {
            return Err(Status::Error);
        }
        // Jᵀ · seed: for sum = a + b the transpose Jacobian is a column of
        // ones, so each output sensitivity equals the single seed entry.
        let s = seed.first().copied().unwrap_or(0.0);
        let sensitivity: Vec<f64> = knowns
            .iter()
            .map(|vr| match *vr {
                VR_INPUT_A | VR_INPUT_B => s,
                _                       => 0.0,
            })
            .collect();
        Ok(sensitivity)
    }

    // ── Clock interval / shift queries – not applicable ──────────────────────

    fn get_interval_decimal(&self, _: Vec<u32>) -> Result<Vec<(f64, IntervalQualifier)>, Status> {
        Err(Status::Error)
    }
    fn get_interval_fraction(&self, _: Vec<u32>) -> Result<Vec<(IntervalFraction, IntervalQualifier)>, Status> {
        Err(Status::Error)
    }
    fn get_shift_decimal(&self, _: Vec<u32>) -> Result<Vec<f64>, Status> {
        Err(Status::Error)
    }
    fn get_shift_fraction(&self, _: Vec<u32>) -> Result<Vec<IntervalFraction>, Status> {
        Err(Status::Error)
    }
    fn set_interval_decimal(&self, _: Vec<u32>, _: Vec<f64>) -> Status {
        Status::Error
    }
    fn set_interval_fraction(&self, _: Vec<u32>, _: Vec<IntervalFraction>) -> Status {
        Status::Error
    }
    fn set_shift_decimal(&self, _: Vec<u32>, _: Vec<f64>) -> Status {
        Status::Error
    }
    fn set_shift_fraction(&self, _: Vec<u32>, _: Vec<IntervalFraction>) -> Status {
        Status::Error
    }
    fn evaluate_discrete_states(&self) -> Status {
        Status::Error
    }

    // ── Co-simulation-specific methods ──────────────────────────────────────

    fn do_step(
        &self,
        current_communication_point:             f64,
        communication_step_size:                 f64,
        _no_set_fmu_state_prior_to_current_point: bool,
    ) -> Result<DoStepResult, Status> {
        let next_time = current_communication_point + communication_step_size;
        store_set(self.idx, |d| {
            d.current_time = next_time;
            d.output_sum = d.input_a + d.input_b;
        });
        Ok(DoStepResult {
            last_successful_time:  next_time,
            event_handling_needed: false,
            terminate_simulation:  false,
            early_return:          false,
        })
    }

    fn set_input_derivatives(
        &self,
        _requests: Vec<(u32, u32)>,
        _values:   Vec<f64>,
    ) -> Status {
        Status::Ok
    }

    fn get_output_derivatives(
        &self,
        _requests: Vec<(u32, u32)>,
    ) -> Result<Vec<f64>, Status> {
        Err(Status::Error)
    }

    fn enter_step_mode(&self) -> Status {
        Status::Ok
    }
}

// ---------------------------------------------------------------------------
// World binding
// ---------------------------------------------------------------------------

struct AdderFmu;

impl CommonGuest for AdderFmu {
    fn get_version() -> String {
        "3.0".to_string()
    }
}

impl CoSimulationGuest for AdderFmu {
    type CoSimulationInstance = AdderCoSimInstance;
}

export!(AdderFmu);
