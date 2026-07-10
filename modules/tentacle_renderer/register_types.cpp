/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "register_types.h"

#include "tentacle_cluster.h"
#include "tentacle_renderer.h"

#include "core/object/class_db.h"

void initialize_tentacle_renderer_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	GDREGISTER_CLASS(TentacleRenderer);
	GDREGISTER_CLASS(TentacleCluster);
}

void uninitialize_tentacle_renderer_module(ModuleInitializationLevel p_level) {
}
