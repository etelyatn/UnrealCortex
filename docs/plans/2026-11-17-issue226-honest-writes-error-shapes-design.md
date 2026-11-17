# Issue 226: Honest property writes, bounded instanced reads, one error code per condition, one MCP error shape

Request: https://github.com/etelyatn/UnrealCortex/issues/226, reported by @XpycT from a source reading of `f72a0ad` (0.4.6).

## Outcome and boundary

A write that cannot apply the caller's value fails with an error instead of reporting success. A legacy property read of an instanced object graph terminates. The same failure condition returns the same native error code in every domain. Every public MCP tool reports a failure in one JSON shape and sets `isError`. No new commands. Response contracts change only for inputs that previously failed silently, for the renamed codes listed below, and for the removed `editor.start_pie` parameters.

## Source basis (base `606bf439`, 0.4.14)

Every point in the report still held. Line numbers in the report have drifted; current owners:

1. `FCortexSerializer::JsonToProperty` (`CortexSerializer.cpp`): bool/int/int64/float/double/byte/string/name branches read `AsBool`/`AsNumber`/`AsString` without checking the JSON type, so `"abc"` or `{}` writes 0/""/false and returns true. Numeric enum values skip the valid-value check; a byte enum above 255 wraps. The map branch ignores the key `ImportText_Direct` result and the value conversion result and always returns true. The array branch retires the existing instanced elements and overwrites the live array before converting the new elements, so a failed element loses the old contents.
2. `PropertyToJson` expands every `CPF_InstancedReference` object through `StructToJson` with no depth bound. In UE 5.8 `UWidget::Slot`, `UPanelSlot::Content`, `UPanelSlot::Parent` and `UPanelWidget::Slots` are all `UPROPERTY(Instanced)` and `Content->Slot` points back at the slot, so `umg.get_property {property_path: "Slot"}` recurses through `Slot -> Content -> Slot`.
3. `umg.set_property` writes only when `value` is present and drops serializer warnings. `level.set_transform` ignores `ParseVectorField` failures; `TryParseVector` (used by `spawn_actor`) accepts a present non-array value as absent. `blueprint.add_scs_component` hard-codes `compiled: true`. `graph.set_pin_value` assigns `Pin->DefaultValue` with no schema validation. `editor.start_pie` declares `map_name` and `spawn_player` but reads only `mode`.
4. Codes: a missing required parameter returns `INVALID_FIELD` (about 212 sites), `INVALID_PARAMETER`, `INVALID_VALUE` or `ACTOR_NOT_FOUND`. A Blueprint path that does not resolve returns `BLUEPRINT_NOT_FOUND` from Blueprint/UMG and `ASSET_NOT_FOUND` from `FCortexGraphNodeOps::LoadBlueprint`. A failed package save returns `SAVE_FAILED`, `SERIALIZATION_ERROR` or `INVALID_OPERATION`.
5. Python: tools return plain `Error: ...` text, `{_error,_message}`, `{success:false,_error,_message}`, `{success:false,error}`, `{error}`, `{error,message}`, or let `UECommandError`/`ConnectionError` escape. `ToolOutputRegistrar._build_result` never sets `isError`. Reflect standalone tools catch only `ConnectionError`, so `query_class_detail("Nope")` raises and loses the editor's `suggestions`.

## Selected design

### Serializer (CortexCore)

- Each scalar branch gates on the JSON type before reading: bool needs `Boolean`; string and name need `String`; numeric branches need `Number`, or a `String` that parses completely as a number. Numbers are then range-checked for the target type (int32, int64, uint8), so out-of-range values are refused instead of being cast with undefined behaviour. A refusal appends a warning that names the property, the expected JSON type and the received JSON type, and returns false. The engine's typed accessors are not sufficient alone: in UE 5.8 `TJsonValueString::TryGetNumber(int32&)` returns true with 0 for `"abc"`, and `FJsonValueNumber::TryGetString`/`TryGetBool` coerce.
- Numeric enum values must satisfy `UEnum::IsValidEnumValueOrBitfield` (equivalent to `IsValidEnumValue` for ordinary enums; flag combinations stay valid); a byte enum value must also fit `uint8`. The refusal warning lists the valid names, like the existing name path.
- The same type gate applies to the reference-like branches that read `AsString()`: `FGameplayTag`, `FSoftObjectPath`, hard object/class references and soft object/class references require a JSON `String`. Null and empty-string clears keep their current meaning.
- Map: a key that `ImportText_Direct` cannot parse or a value that fails conversion fails the whole write.
- An instanced element whose `properties` fail to convert fails the element (`JsonToInstancedSubObject` propagates the `JsonToStruct` result).
- Array, map and `FGameplayTagContainer` writes are all-or-nothing: the new contents are converted into a temporary value first; only on success is the live value replaced and the replaced owned instanced elements retired (`RetireOwnedSubObject`). On failure the live value and its sub-objects are untouched. Positional array-to-struct promotion follows the same rule. `JsonToInstancedSubObject` builds the new object before it touches the slot.
- `PropertyToJson` instanced expansion is bounded. It tracks the objects being expanded on the current path and a depth limit of `FCortexSerializationPolicy::MaxDepth` (8). An object already being expanded, or one beyond the depth limit, is emitted as its object path string (the non-instanced reference representation), so cycles terminate and output stays finite.

### Domain writes

- `umg.set_property` and `umg.get_property` use `StrictParams()`; a missing `value` returns `INVALID_FIELD`. Serializer warnings are returned in `warnings` on success and in the message and `details.warnings` on refusal.
- `level.set_transform` fails with `INVALID_VALUE` when a present `location`/`rotation`/`scale` is not a 3-number array, before any change. `TryParseVector` refuses a present non-array value, so `spawn_actor` and `duplicate_actor` agree. Vector elements must be numbers.
- `blueprint.add_scs_component` reports `compiled` from the actual compile result (`IsBlueprintCompiled`), like `set_component_defaults`.
- `graph.set_pin_value` validates the new default with the engine schema (`UEdGraphSchema::IsPinDefaultValid`) before writing and returns `INVALID_VALUE` with the schema's message when it refuses. No pin is modified on refusal. Class, object and interface pin values are resolved to `DefaultObject` (guarded load); soft references are stored as canonical paths; an unresolved path is refused instead of being stored as a raw string.
- `editor.start_pie` no longer declares `map_name` and `spawn_player` (AGENTS.md: do not expose unimplemented parameters).

### Error-code rules (native)

1. A missing required parameter returns `INVALID_FIELD`.
2. A Blueprint asset path that does not resolve returns `BLUEPRINT_NOT_FOUND`, including in Graph. Non-Blueprint assets (maps, macros, generic assets) keep `ASSET_NOT_FOUND`.
3. A failed package/map save returns `SAVE_FAILED`. `SERIALIZATION_ERROR` stays for JSON/text serialization and non-save failures (for example deletion, per the issue 218 design). `INVALID_OPERATION` stays for refused option combinations such as "save=true requires compile=true". Commands that already report an applied mutation with `saved:false` keep that contract.

Found during the fix: a `UPackage::SavePackage` call without `SAVE_NoError` writes a refused save through `FSavePackageArgs::Error`, which defaults to `GError`. The editor then raises a fatal error instead of returning (`SavePackage2.cpp`, observed as a Critical error in the RED runs). Every plugin package save now passes `SAVE_NoError`, so the failure reaches the caller. The `saved:false` Blueprint settings commands also add the save failure to `warnings`.

No aliases (AGENTS.md: no backward-compatibility shims). Tests and docs that pin the old codes change with the code.

### MCP error shape (Python)

The canonical failure shape is the one `mcp-tool-reference.md` already documents: `{"success": false, "_error": CODE, "_message": MESSAGE, ...details}` (plus `_command` where the router knows it). `ToolOutputRegistrar`:

- sets `isError=True` whenever the decoded result is a dict with a non-empty `_error`;
- converts `UECommandError` raised by a tool into the canonical shape with the editor's code, message and non-reserved details, and `ConnectionError` into `CONNECTION_ERROR`, both with `isError=True`. Other exceptions still propagate (programming errors).

Public tools that return another failure shape are changed at their source to return the canonical shape. Non-JSON text is not parsed for codes. Reflect standalone tools then return the editor's `suggestions` through the registrar.

## Acceptance

- Writing `"abc"`, `{}` or an out-of-range integer to an int/float/double/byte/bool/string/name property fails with a warning and leaves the value unchanged; `"5"` into an int still writes 5.
- A numeric enum value outside the enum fails. A map with an unparsable key or failing value fails. A failing array or map write leaves the previous elements and instanced sub-objects intact.
- `umg.get_property {property_path: "Slot"}` on a widget in a panel returns a finite result.
- `umg.set_property` without `value` returns `INVALID_FIELD`; an unknown enum name returns the valid names in `warnings` or the error.
- `level.set_transform` with `location: [1,2]` or `location: "x"` returns `INVALID_VALUE` and the transform is unchanged; `spawn_actor` with `location: "x"` is refused.
- `add_scs_component` on a Blueprint that fails to compile reports `compiled: false`.
- `graph.set_pin_value` of `"abc"` on a float pin returns `INVALID_VALUE` and the default is unchanged.
- `editor.start_pie` schema has only `mode`.
- The renamed sites return the rule's code; tests pin the new codes.
- Every public MCP tool failure decodes to `_error`/`_message` with `isError=True`; `query_class_detail("Nope")` returns the editor's code and `suggestions` instead of raising.
