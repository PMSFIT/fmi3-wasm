# FMI 3.0 → WebAssembly Component Model Mapping

WIT definitions that map the [FMI 3.0](https://svn.modelica.org/fmi/branches/public/specifications/v3.0/) C API to the [WebAssembly Component Model](https://component-model.bytecodealliance.org/) interface description language (WIT).

## File layout

```
wit/
├── fmi3-types.wit              # Primitive types, enums, records
├── fmi3-callbacks.wit          # Environment → FMU callbacks
├── fmi3-common.wit             # get-version free function only
├── fmi3-model-exchange.wit     # Standalone ME instance resource + all methods
├── fmi3-co-simulation.wit      # Standalone CS instance resource + all methods
├── fmi3-scheduled-execution.wit# Standalone SE instance resource + all methods
└── world.wit                   # Composed worlds for each FMU kind
```

## Quick validation

```sh
# Requires wasm-tools ≥ 1.200
wasm-tools component wit wit/
```

## Design decisions

### `fmi3Instance` → three standalone resources

The opaque C pointer is mapped to three distinct WIT resources — one per
interface kind:

| FMU kind           | WIT resource                    | Static factory                     |
|--------------------|----------------------------------|-------------------------------------|
| Model Exchange     | `model-exchange-instance`        | `instantiate-model-exchange`        |
| Co-Simulation      | `co-simulation-instance`         | `instantiate-co-simulation`         |
| Scheduled Execution| `scheduled-execution-instance`   | `instantiate-scheduled-execution`   |

Each resource owns the full lifecycle of the FMU instance: it carries the
instantiation factory as a static method, all common lifecycle and variable
access methods (`enter-initialization-mode`, `get-float64`, `set-fmu-state`,
etc.), and the interface-kind-specific methods (`do-step`, `set-time`, …).

Resource ownership is linear: `fmi3FreeInstance` is called when the resource
is dropped, so no explicit free binding is needed.

This design means the host never holds an intermediate `common::instance`
handle: instantiation and all operations go through the single typed resource,
making it impossible to call ME-only methods (e.g. `set-time`) on a CS handle.

### Callback pointers → imported interfaces

FMI 3.0 passes callbacks as function pointers to `fmi3Instantiate*`.  In the
Component Model these become **imported** interfaces supplied by the host world.
The `fmi3InstanceEnvironment` opaque pointer is dropped entirely; the
Component Model's closure semantics handle per-instance context automatically.

### `intermediateUpdateCallback` is optional

The `intermediate-update-callbacks` interface is a separate import so that
simple CS FMUs that do not support early return can omit it.

### Parallel C arrays → `list<T>` / `list<tuple<…>>`

FMI getter/setter functions use parallel C arrays (pointer + size_t length).
These map to `list<T>` in WIT.  Where two arrays are logically paired (e.g.
`vrs[]` + `orders[]` for output derivatives), they are collapsed into
`list<tuple<value-reference, u32>>` to prevent length mismatches.

### Optional parameters → `option<T>`

`fmi3EnterInitializationMode` takes a `toleranceDefined` boolean alongside
`tolerance`, and similarly for `stopTimeDefined` / `stopTime`.  These pairs
collapse to `option<f64>` in WIT, which is both more idiomatic and eliminates
the possibility of passing `toleranceDefined = false` with a non-zero
`tolerance`.

### Output parameters → return values / records

Functions with multiple C out-parameters (e.g. `fmi3UpdateDiscreteStates`,
`fmi3DoStep`, `fmi3CompletedIntegratorStep`) return a record instead of
multiple pointer arguments.  This is idiomatic for WIT and eliminates
null-pointer risks.

### `fmi3FMUState` → `list<u8>`

The C API uses a two-phase serialise approach (`fmi3GetFMUState` +
`fmi3SerializeFMUState`).  The WIT mapping merges these into a single
`get-fmu-state` that returns `list<u8>` (serialised bytes immediately).
The host therefore never holds an un-serialised `fmi3FMUState` handle.

### `fmi3Boolean` → `bool`

FMI 3.0 defines `fmi3Boolean` as `int`-sized (for C ABI compatibility).
The Component Model uses a native 1-byte `bool`, which is correct for WIT.

### `fmi3String` → `string`

FMI strings are null-terminated UTF-8.  WIT `string` is length-prefixed UTF-8;
the null terminator is elided.

### `fmi3Binary` → `list<u8>`

Binary variables carry an explicit length in C (`valueSizes[]`).  In WIT each
blob is a `list<u8>` whose length is intrinsic, so the size array is removed.
For `fmi3GetBinary` / `fmi3SetBinary` dealing with multiple variables the
type becomes `list<list<u8>>`.

## Type mapping table

| FMI 3.0 C type           | WIT type          | Notes                              |
|--------------------------|-------------------|------------------------------------|
| `fmi3ValueReference`     | `u32`             | Direct alias                       |
| `fmi3Float64`            | `f64`             | IEEE 754 double                    |
| `fmi3Float32`            | `f32`             | IEEE 754 single                    |
| `fmi3Int8/16/32/64`      | `s8/s16/s32/s64`  | Signed integers                    |
| `fmi3UInt8/16/32/64`     | `u8/u16/u32/u64`  | Unsigned integers                  |
| `fmi3Boolean`            | `bool`            | C int-sized → WIT bool             |
| `fmi3String`             | `string`          | Null-terminator elided             |
| `fmi3Binary`             | `list<u8>`        | Length-prefixed blob               |
| `fmi3Clock`              | `bool`            | true = tick                        |
| `fmi3FMUState`           | `list<u8>`        | Serialised immediately             |
| `fmi3Instance`           | `resource model-exchange-instance` / `co-simulation-instance` / `scheduled-execution-instance` | Owned handle; one resource per interface kind |
| `fmi3Status`             | `enum status`     | Five variants                      |
| `fmi3IntervalQualifier`  | `enum interval-qualifier` | Three variants             |
| `fmi3DependencyKind`     | `enum dependency-kind` | Five variants                 |
| Callback function pointer| imported `interface callbacks` | Host-supplied        |

## Known limitations and future work

1. **`fmi3GetVersion` is a free function** — it is the only item in the
   `common` interface, exposed at the interface level rather than as a resource
   method, matching the C API.

2. **Model description XML** — FMI 3.0 ships metadata as `modelDescription.xml`
   inside the FMU archive.  This mapping covers only the runtime API; the
   static metadata is out of scope.

3. **Threading model for SE** — The preemption lock/unlock callbacks are mapped
   as host-imported functions, but the Component Model threading extensions
   (WASI threads or `shared-everything-threads`) are still evolving.  Platform
   integrators may need to adapt when those proposals stabilise.

4. **`fmi3Char`** — Used only in `fmi3GetVersion` return value; mapped to
   `string`.

5. **Serialise/deserialise split** — If round-tripping large state blobs proves
   expensive, the two-phase C API (`GetFMUState` + `SerializeFMUState`) can be
   re-exposed as separate WIT functions without changing the type mapping.
