class_name DemoSpinner
extends MeshInstance3D

## Simple custom class for exercising project-script calls over MCP:
## typed/untyped echoes, collection returns and property writes.

var speed := 1.0
var counter := 0

func _process(delta: float) -> void:
	rotate_y(delta * speed)

func bump(amount: int = 1) -> int:
	counter += amount
	return counter

func echo_text(value: String) -> String:
	return value

func echo_vector(value: Vector3) -> Vector3:
	return value

func children_of(node: Node) -> Array[Node]:
	return node.get_children()

func make_points(count: int) -> Array[Vector3]:
	var points: Array[Vector3] = []
	for i in count:
		points.append(Vector3(i, i * 0.5, -i))
	return points
