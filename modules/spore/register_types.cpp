/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "register_types.h"

#include "spore_grid.h"
#include "spore_manager.h"

#include "core/object/class_db.h"

void initialize_spore_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	GDREGISTER_CLASS(SporeGrid);
	GDREGISTER_CLASS(SporeManager);
}

void uninitialize_spore_module(ModuleInitializationLevel p_level) {
}
