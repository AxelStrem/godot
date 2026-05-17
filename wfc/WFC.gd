extends WFCSolver

func _enter_tree() -> void:
	if catalog_set == null:
		catalog_set = load("res://wfc/catalogs/default_wfc_catalog_set.tres")

func add_chamber(root: Node) -> void:
	add_branch(root)
