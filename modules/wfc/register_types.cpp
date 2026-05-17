/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "register_types.h"

#include "wfc_solver.h"
#include "wfc_types.h"

#include "core/object/class_db.h"

void initialize_wfc_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	GDREGISTER_CLASS(WFCCatalog);
	GDREGISTER_CLASS(WFCCatalogSet);
	GDREGISTER_CLASS(WFCParam);
	GDREGISTER_CLASS(WFCNeighbor);
	GDREGISTER_CLASS(WFCElement);
	GDREGISTER_CLASS(WFCSolver);
}

void uninitialize_wfc_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}