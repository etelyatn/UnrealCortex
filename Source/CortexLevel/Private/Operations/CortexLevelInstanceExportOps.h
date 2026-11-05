#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;

/**
 * level.export_class_instances: reads every placed instance of an actor class (subclasses included) out of
 * saved map packages and writes the editable properties that differ from the class default (changed_only,
 * default) or all editable properties to a report file.
 *
 * Maps are read WITHOUT a world transition: each map package is loaded as data (not opened as the editor
 * world), its persistent level actors are iterated, and packages this call loaded are released again, so the
 * active editor world, its dirty state and the map files stay untouched. A map whose actors live in external
 * actor packages (One File Per Actor / World Partition) cannot be read that way and is refused, never
 * exported as if it had no instances.
 */
class FCortexLevelInstanceExportOps
{
public:
	/** Params: class (string), maps (non-empty array of map package paths), changed_only (bool, default true), out_path (string). */
	static FCortexCommandResult ExportClassInstances(const TSharedPtr<FJsonObject>& Params);
};
