extends WFCSolver

func _enter_tree() -> void:
	if catalog_set == null:
		catalog_set = load("res://wfc/catalogs/default_wfc_catalog_set.tres")
	if graph_processor == null and ResourceLoader.exists("res://wfc/default_wfc_graph_processor.gd"):
		graph_processor = load("res://wfc/default_wfc_graph_processor.gd").new()

func add_chamber(root: Node) -> void:
	add_branch(root)
