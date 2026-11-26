#include "value_codec.hpp"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <charconv>
#include <cmath>
#include <limits>

using namespace godot;

namespace godot_cm {

[[noreturn]] void ValueCodec::fail(const String &code, const String &message, String path, const String &expected, const String &received) {
	Dictionary diagnostic;
	diagnostic["code"] = code;
	diagnostic["message"] = message;
	diagnostic["phase"] = "conversion";
	diagnostic["path"] = path;
	if (!expected.is_empty()) diagnostic["expected"] = expected;
	if (!received.is_empty()) diagnostic["received"] = received;
	throw ValueFailure(diagnostic);
}

int ValueCodec::type_id(const Dictionary &info) {
	String name = info.get("type", "Variant");
	if (name == "Variant" || name == "void" || info.get("variant_type", Variant()).get_type() == Variant::NIL) return -1;
	int type = info.get("variant_type", -1);
	return type >= 0 && type < Variant::VARIANT_MAX ? type : -1;
}

Dictionary ValueCodec::descriptor(const String &name) {
	Dictionary result;
	result["type"] = name;
	for (int i = 0; i < Variant::VARIANT_MAX; i++) {
		if (name == Variant::get_type_name((Variant::Type)i)) {
			result["variant_type"] = i;
			return result;
		}
	}
	if (name == "Variant" || name == "void") {
		result["variant_type"] = Variant();
		return result;
	}
	if (!name.is_valid_identifier() && !name.begins_with("script:res://")) fail("INVALID_VALUE_ENCODING", "invalid type descriptor", "type");
	result["variant_type"] = (int)Variant::OBJECT;
	result["class_name"] = name;
	return result;
}

std::vector<ValueField> ValueCodec::fields(int type) {
	switch (type) {
		case Variant::VECTOR2: return {{"x", Variant::FLOAT}, {"y", Variant::FLOAT}};
		case Variant::VECTOR2I: return {{"x", Variant::INT}, {"y", Variant::INT}};
		case Variant::VECTOR3: return {{"x", Variant::FLOAT}, {"y", Variant::FLOAT}, {"z", Variant::FLOAT}};
		case Variant::VECTOR3I: return {{"x", Variant::INT}, {"y", Variant::INT}, {"z", Variant::INT}};
		case Variant::VECTOR4: case Variant::QUATERNION: return {{"x", Variant::FLOAT}, {"y", Variant::FLOAT}, {"z", Variant::FLOAT}, {"w", Variant::FLOAT}};
		case Variant::VECTOR4I: return {{"x", Variant::INT}, {"y", Variant::INT}, {"z", Variant::INT}, {"w", Variant::INT}};
		case Variant::COLOR: return {{"r", Variant::FLOAT}, {"g", Variant::FLOAT}, {"b", Variant::FLOAT}, {"a", Variant::FLOAT, true}};
		case Variant::RECT2: return {{"position", Variant::VECTOR2}, {"size", Variant::VECTOR2}};
		case Variant::RECT2I: return {{"position", Variant::VECTOR2I}, {"size", Variant::VECTOR2I}};
		case Variant::AABB: return {{"position", Variant::VECTOR3}, {"size", Variant::VECTOR3}};
		case Variant::PLANE: return {{"normal", Variant::VECTOR3}, {"d", Variant::FLOAT}};
		case Variant::TRANSFORM2D: return {{"x", Variant::VECTOR2}, {"y", Variant::VECTOR2}, {"origin", Variant::VECTOR2}};
		case Variant::BASIS: return {{"x", Variant::VECTOR3}, {"y", Variant::VECTOR3}, {"z", Variant::VECTOR3}};
		case Variant::TRANSFORM3D: return {{"basis", Variant::BASIS}, {"origin", Variant::VECTOR3}};
		case Variant::PROJECTION: return {{"x", Variant::VECTOR4}, {"y", Variant::VECTOR4}, {"z", Variant::VECTOR4}, {"w", Variant::VECTOR4}};
		default: return {};
	}
}

static Dictionary tagged(const String &type) {
	Dictionary value;
	value["$t"] = type;
	return value;
}

static int packed_element(int type) {
	switch (type) {
		case Variant::PACKED_BYTE_ARRAY: case Variant::PACKED_INT32_ARRAY: case Variant::PACKED_INT64_ARRAY: return Variant::INT;
		case Variant::PACKED_FLOAT32_ARRAY: case Variant::PACKED_FLOAT64_ARRAY: return Variant::FLOAT;
		case Variant::PACKED_STRING_ARRAY: return Variant::STRING;
		case Variant::PACKED_VECTOR2_ARRAY: return Variant::VECTOR2;
		case Variant::PACKED_VECTOR3_ARRAY: return Variant::VECTOR3;
		case Variant::PACKED_COLOR_ARRAY: return Variant::COLOR;
		case Variant::PACKED_VECTOR4_ARRAY: return Variant::VECTOR4;
		default: return -1;
	}
}

static bool reserved(const Dictionary &value) {
	return value.has("$t") || value.has("__godotHandle") || value.has("__godotValue") || value.has("$handle");
}

static void exact_keys(const Dictionary &data, const Array &allowed, const Array &required, String path) {
	for (const Variant &key : data.keys()) {
		if (key.get_type() != Variant::STRING || !allowed.has(key)) ValueCodec::fail("INVALID_VALUE_ENCODING", "unknown field", path + "." + String(key));
	}
	for (const Variant &key : required) {
		if (!data.has(key)) ValueCodec::fail("INVALID_VALUE_ENCODING", "required field missing", path + "." + String(key));
	}
}

int64_t ValueCodec::integer_text(const String &text, String path) {
	CharString bytes = text.utf8();
	int64_t result = 0;
	const char *begin = bytes.get_data(), *end = begin + bytes.length();
	auto parsed = std::from_chars(begin, end, result);
	if (bytes.length() == 0 || bytes.length() > 20 || parsed.ec != std::errc() || parsed.ptr != end || String::num_int64(result) != text) {
		fail("TYPE_MISMATCH", "expected canonical signed-64 decimal integer", path, "int64", "String");
	}
	return result;
}

static Variant empty_math(int type) {
	switch (type) {
		case Variant::VECTOR2: return Vector2(); case Variant::VECTOR2I: return Vector2i();
		case Variant::VECTOR3: return Vector3(); case Variant::VECTOR3I: return Vector3i();
		case Variant::VECTOR4: return Vector4(); case Variant::VECTOR4I: return Vector4i();
		case Variant::RECT2: return Rect2(); case Variant::RECT2I: return Rect2i();
		case Variant::PLANE: return Plane(); case Variant::QUATERNION: return Quaternion();
		case Variant::AABB: return AABB(); case Variant::BASIS: return Basis();
		case Variant::TRANSFORM2D: return Transform2D(); case Variant::TRANSFORM3D: return Transform3D();
		case Variant::PROJECTION: return Projection(); case Variant::COLOR: return Color();
		default: ValueCodec::fail("UNSUPPORTED_VARIANT_TYPE", "no named structure for type", "type");
	}
}

static Variant make_packed(int type, const Array &items) {
	switch (type) {
		case Variant::PACKED_BYTE_ARRAY: return PackedByteArray(items);
		case Variant::PACKED_INT32_ARRAY: return PackedInt32Array(items);
		case Variant::PACKED_INT64_ARRAY: return PackedInt64Array(items);
		case Variant::PACKED_FLOAT32_ARRAY: return PackedFloat32Array(items);
		case Variant::PACKED_FLOAT64_ARRAY: return PackedFloat64Array(items);
		case Variant::PACKED_STRING_ARRAY: return PackedStringArray(items);
		case Variant::PACKED_VECTOR2_ARRAY: return PackedVector2Array(items);
		case Variant::PACKED_VECTOR3_ARRAY: return PackedVector3Array(items);
		case Variant::PACKED_COLOR_ARRAY: return PackedColorArray(items);
		case Variant::PACKED_VECTOR4_ARRAY: return PackedVector4Array(items);
		default: ValueCodec::fail("UNSUPPORTED_VARIANT_TYPE", "unknown packed type", "type");
	}
}

Variant ValueCodec::native_checked(const Variant &value, const Dictionary &expected, String path, int depth) {
	int target = type_id(expected), actual = value.get_type();
	if (target < 0) return value;
	if (value.get_type() == Variant::NIL && target == Variant::OBJECT) return value;
	if (actual != target) fail("TYPE_MISMATCH", "value type does not match destination", path, expected.get("type", ""), Variant::get_type_name(value.get_type()));
	if (target == Variant::OBJECT) {
		Object *object = value.get_validated_object();
		String cls = expected.get("class_name", "");
		if (!object) fail("INVALID_HANDLE", "object is no longer valid", path);
		if (!cls.is_empty() && (!object_matches || !object_matches(object, cls))) fail("TYPE_MISMATCH", "incompatible object class", path, cls, object->get_class());
	}
	if (depth > MAX_DEPTH) fail("VALUE_DEPTH_LIMIT", "value nesting limit exceeded", path);
	auto verify = [&](const char *key, int builtin, const StringName &cls, const Variant &script) {
		if (!expected.has(key) || type_id(expected[key]) < 0) return;
		Dictionary actual_info = collection_type ? collection_type(builtin, cls, script) : descriptor(builtin == Variant::NIL ? String("Variant") : builtin == Variant::OBJECT ? String(cls) : Variant::get_type_name((Variant::Type)builtin));
		if (actual_info.is_empty() || type_id(actual_info) < 0) fail("TYPE_MISMATCH", "retained collection lacks the required native type constraint", path + "." + key);
		Dictionary declared; declared[key] = actual_info;
		collection_constraint(expected, declared, key, path);
	};
	if (target == Variant::ARRAY) {
		Array items = value;
		verify("element_type", items.get_typed_builtin(), items.get_typed_class_name(), items.get_typed_script());
	}
	if (target == Variant::DICTIONARY) {
		Dictionary items = value;
		verify("key_type", items.get_typed_key_builtin(), items.get_typed_key_class_name(), items.get_typed_key_script());
		verify("value_type", items.get_typed_value_builtin(), items.get_typed_value_class_name(), items.get_typed_value_script());
	}
	return value;
}

Dictionary ValueCodec::collection_constraint(const Dictionary &destination, const Dictionary &explicit_info, const char *key, String path) {
	Dictionary expected = destination.get(key, Dictionary());
	if (!explicit_info.has(key)) return expected;
	Variant raw = explicit_info[key];
	Dictionary supplied;
	if (raw.get_type() == Variant::STRING) supplied = descriptor(raw);
	else if (raw.get_type() == Variant::DICTIONARY && ((Dictionary)raw).get("type", Variant()).get_type() == Variant::STRING) {
		supplied = descriptor(((Dictionary)raw)["type"]);
		Dictionary source = raw;
		if (source.has("class_name") && source["class_name"] != supplied.get("class_name", "")) fail("TYPE_MISMATCH", "contradictory collection class descriptor", path + "." + key);
		if (source.has("variant_type") && type_id(source) != type_id(supplied)) fail("TYPE_MISMATCH", "contradictory collection type descriptor", path + "." + key);
	} else fail("INVALID_VALUE_ENCODING", "expected type name or descriptor", path + "." + key);
	if (!expected.is_empty() && (type_id(expected) != type_id(supplied) || expected.get("class_name", "") != supplied.get("class_name", ""))) fail("TYPE_MISMATCH", "explicit collection constraint conflicts with destination", path + "." + key);
	return expected.is_empty() ? supplied : expected;
}

Variant ValueCodec::decode(const Variant &wire, const Dictionary &expected, String path, int depth) {
	if (depth > MAX_DEPTH || ++visited > 100000) fail("VALUE_DEPTH_LIMIT", "value nesting or element limit exceeded", path);
	int target = type_id(expected), source = wire.get_type();
	Dictionary info;
	bool explicit_type = false;
	if (source == Variant::DICTIONARY) {
		info = wire;
		if (info.has("__godotHandle") || info.has("__godotValue") || info.has("$handle")) {
			if (!resolve) fail("INVALID_VALUE_REFERENCE", "references are unavailable", path);
			return native_checked(resolve(info, path), expected, path, depth);
		}
		if (info.has("$t")) {
			if (info["$t"].get_type() != Variant::STRING) fail("INVALID_VALUE_ENCODING", "type tag must be a string", path + ".$t");
			String name = info["$t"];
			int tagged_type = -1;
			for (int i = 0; i < Variant::VARIANT_MAX; i++) if (name == Variant::get_type_name((Variant::Type)i)) { tagged_type = i; break; }
			if (tagged_type < 0) fail("UNSUPPORTED_VARIANT_TYPE", "unknown explicit Variant type", path + ".$t");
			if (target >= 0 && target != tagged_type) fail("TYPE_MISMATCH", "explicit type conflicts with destination", path, expected.get("type", ""), name);
			target = tagged_type;
			explicit_type = true;
		}
	}
	if (explicit_type && (target <= Variant::STRING || target == Variant::STRING_NAME || target == Variant::NODE_PATH)) {
		exact_keys(info, Array::make("$t", "value"), Array::make("$t", "value"), path);
		Variant value = info["value"];
		if (target == Variant::INT) {
			if (value.get_type() != Variant::STRING) fail("TYPE_MISMATCH", "integer tag requires decimal string", path + ".value", "String", Variant::get_type_name(value.get_type()));
			return integer_text(value, path + ".value");
		}
		if (target == Variant::FLOAT && value.get_type() == Variant::STRING) {
			String special = value;
			if (special == "NaN") return std::numeric_limits<double>::quiet_NaN();
			if (special == "+Infinity") return std::numeric_limits<double>::infinity();
			if (special == "-Infinity") return -std::numeric_limits<double>::infinity();
			if (special == "-0") return -0.0;
			fail("INVALID_VALUE_ENCODING", "unknown exceptional float", path + ".value");
		}
		return decode(value, descriptor(Variant::get_type_name((Variant::Type)target)), path + ".value", depth + 1);
	}
	if (source == Variant::NIL) {
		if (target < 0 || target == Variant::NIL || target == Variant::OBJECT) return wire;
		fail("TYPE_MISMATCH", "null is not valid for this destination", path, expected.get("type", ""), "Nil");
	}
	if (target == Variant::INT) {
		if (source == Variant::INT) return wire;
		if (source == Variant::FLOAT && std::isfinite((double)wire) && std::floor((double)wire) == (double)wire && std::abs((double)wire) <= SAFE_INTEGER) return (int64_t)(double)wire;
		fail("TYPE_MISMATCH", "expected exact safe integer or explicit int64 tag", path, "int", Variant::get_type_name(wire.get_type()));
	}
	if (target == Variant::FLOAT && (source == Variant::INT || source == Variant::FLOAT)) return (double)wire;
	if ((target == Variant::STRING_NAME || target == Variant::NODE_PATH) && source == Variant::STRING) return target == Variant::STRING_NAME ? Variant(StringName(String(wire))) : Variant(NodePath(String(wire)));
	std::vector<ValueField> structure = fields(target);
	if (!structure.empty()) {
		if (source != Variant::DICTIONARY) fail("TYPE_MISMATCH", "expected named component object", path, Variant::get_type_name((Variant::Type)target), Variant::get_type_name(wire.get_type()));
		Array allowed, required;
		if (explicit_type) allowed.push_back("$t");
		for (const ValueField &field : structure) {
			allowed.push_back(field.name);
			if (!field.optional) required.push_back(field.name);
		}
		exact_keys(info, allowed, required, path);
		Variant result = empty_math(target);
		for (const ValueField &field : structure) {
			String child = path + "." + field.name;
			Variant component = decode(info.get(field.name, 1.0), descriptor(Variant::get_type_name(field.type)), child, depth + 1);
			if (field.type == Variant::INT && ((int64_t)component < INT32_MIN || (int64_t)component > INT32_MAX)) fail("TYPE_MISMATCH", "component outside signed-32 range", child, "int32", "int");
			double max = target == Variant::COLOR ? std::numeric_limits<float>::max() : std::numeric_limits<real_t>::max();
			if (field.type == Variant::FLOAT && (!std::isfinite((double)component) || std::abs((double)component) > max)) fail("TYPE_MISMATCH", "component must be finite and representable", child, "finite number", "float");
			bool valid = false;
			result.set_named(field.name, component, valid);
			if (!valid) fail("INVALID_VALUE_ENCODING", "component could not be assigned", child);
		}
		return result;
	}
	int element = packed_element(target);
	if (target == Variant::ARRAY || element >= 0 || (target < 0 && source == Variant::ARRAY)) {
		Variant raw_items = wire;
		if (explicit_type) {
			exact_keys(info, Array::make("$t", "values", "element_type"), Array::make("$t", "values"), path);
			raw_items = info["values"];
		}
		if (raw_items.get_type() != Variant::ARRAY) fail("TYPE_MISMATCH", "expected array values", path, "Array", Variant::get_type_name(raw_items.get_type()));
		Array input = raw_items, result;
		Dictionary constraint = element >= 0 ? descriptor(Variant::get_type_name((Variant::Type)element)) : collection_constraint(expected, explicit_type ? info : Dictionary(), "element_type", path);
		for (int i = 0; i < input.size(); i++) {
			String child = path + "[" + itos(i) + "]";
			Variant value = decode(input[i], constraint, child, depth + 1);
			if (target == Variant::PACKED_BYTE_ARRAY && ((int64_t)value < 0 || (int64_t)value > 255)) fail("TYPE_MISMATCH", "byte outside 0..255", child, "byte", "int");
			if (target == Variant::PACKED_INT32_ARRAY && ((int64_t)value < INT32_MIN || (int64_t)value > INT32_MAX)) fail("TYPE_MISMATCH", "integer outside signed-32 range", child, "int32", "int");
			if (target == Variant::PACKED_FLOAT32_ARRAY && std::isfinite((double)value) && std::abs((double)value) > std::numeric_limits<float>::max()) fail("TYPE_MISMATCH", "float32 overflow", child, "float32", "float64");
			result.push_back(value);
		}
		if (element >= 0) return make_packed(target, result);
		if (type_id(constraint) >= 0) {
			Dictionary storage = storage_type ? storage_type(constraint) : constraint;
			Array typed;
			typed.set_typed(type_id(constraint), storage.get("class_name", ""), storage.get("script", Variant()));
			typed.assign(result);
			return typed;
		}
		return result;
	}
	if (target == Variant::DICTIONARY || (target < 0 && source == Variant::DICTIONARY)) {
		if (source != Variant::DICTIONARY) fail("TYPE_MISMATCH", "expected dictionary", path, "Dictionary", Variant::get_type_name(wire.get_type()));
		Array entries;
		if (explicit_type) {
			exact_keys(info, Array::make("$t", "entries", "key_type", "value_type"), Array::make("$t", "entries"), path);
			if (info["entries"].get_type() != Variant::ARRAY) fail("INVALID_VALUE_ENCODING", "entries must be an array", path + ".entries");
			entries = info["entries"];
		} else {
			for (const Variant &key : info.keys()) { Dictionary pair; pair["key"] = key; pair["value"] = info[key]; entries.push_back(pair); }
		}
		Dictionary key_type = collection_constraint(expected, explicit_type ? info : Dictionary(), "key_type", path);
		Dictionary value_type = collection_constraint(expected, explicit_type ? info : Dictionary(), "value_type", path);
		Dictionary result;
		for (int i = 0; i < entries.size(); i++) {
			String child = path + ".entries[" + itos(i) + "]";
			if (entries[i].get_type() != Variant::DICTIONARY) fail("INVALID_VALUE_ENCODING", "entry must contain key and value", child);
			Dictionary pair = entries[i];
			exact_keys(pair, Array::make("key", "value"), Array::make("key", "value"), child);
			Variant key = decode(pair["key"], key_type, child + ".key", depth + 1);
			Variant value = decode(pair["value"], value_type, child + ".value", depth + 1);
			if (result.has(key)) fail("TYPE_MISMATCH", "duplicate converted dictionary key", child + ".key");
			result[key] = value;
		}
		if (type_id(key_type) >= 0 || type_id(value_type) >= 0) {
			Dictionary ks = storage_type ? storage_type(key_type) : key_type, vs = storage_type ? storage_type(value_type) : value_type;
			Dictionary typed;
			typed.set_typed(MAX(0, type_id(key_type)), ks.get("class_name", ""), ks.get("script", Variant()), MAX(0, type_id(value_type)), vs.get("class_name", ""), vs.get("script", Variant()));
			typed.assign(result);
			return typed;
		}
		return result;
	}
	if (target == Variant::OBJECT || target == Variant::RID || target == Variant::CALLABLE || target == Variant::SIGNAL) fail("TYPE_MISMATCH", "destination requires a valid opaque reference", path, Variant::get_type_name((Variant::Type)target), Variant::get_type_name(wire.get_type()));
	if (explicit_type) fail("INVALID_VALUE_ENCODING", "invalid explicit type encoding", path);
	return native_checked(wire, expected, path, depth);
}

Dictionary ValueCodec::native_descriptor(const Variant &value) const {
	Dictionary result = descriptor(Variant::get_type_name(value.get_type()));
	if (value.get_type() == Variant::OBJECT) {
		Object *object = value.get_validated_object();
		if (object) { result["type"] = object->get_class(); result["class_name"] = object->get_class(); }
	}
	return result;
}

static bool finite_math(const Variant &value) {
	if (value.get_type() == Variant::FLOAT) return std::isfinite((double)value) && !((double)value == 0 && std::signbit((double)value));
	for (const ValueField &field : ValueCodec::fields(value.get_type())) {
		bool valid = false;
		Variant component = value.get_named(field.name, valid);
		if (!valid || !finite_math(component)) return false;
	}
	return true;
}

static int64_t estimate_data(const Variant &value, int depth, std::vector<Variant> &ancestors) {
	constexpr int MAX_DEPTH = ValueCodec::MAX_DEPTH;
	constexpr int64_t MAX_VALUE_BYTES = ValueCodec::MAX_VALUE_BYTES;
	if (depth > MAX_DEPTH) return 128;
	bool collection = value.get_type() == Variant::ARRAY || value.get_type() == Variant::DICTIONARY;
	if (collection) {
		for (const Variant &ancestor : ancestors) if (UtilityFunctions::is_same(value, ancestor)) return 64;
		ancestors.push_back(value);
	}
	struct Pop { std::vector<Variant> &values; bool pop; ~Pop() { if (pop) values.pop_back(); } } pop{ancestors, collection};
	int type = value.get_type();
	if (type == Variant::STRING || type == Variant::STRING_NAME || type == Variant::NODE_PATH) return 32 + (int64_t)String(value).length() * 4;
	int element = packed_element(type);
	if (element >= 0) {
		Variant sized = value;
		int64_t count = sized.call("size");
		int64_t per_item = element == Variant::STRING ? 32 : element == Variant::VECTOR4 || element == Variant::COLOR ? 16 : element == Variant::VECTOR3 ? 12 : 8;
		if (count > MAX_VALUE_BYTES / per_item) return MAX_VALUE_BYTES + 1;
		if (element != Variant::STRING) return 64 + count * per_item;
		PackedStringArray strings = value;
		int64_t bytes = 64;
		for (int i = 0; i < strings.size() && bytes <= MAX_VALUE_BYTES; i++) bytes += 32 + (int64_t)strings[i].length() * 4;
		return bytes;
	}
	if (type == Variant::ARRAY) {
		Array items = value;
		if (items.size() > MAX_VALUE_BYTES / 32) return MAX_VALUE_BYTES + 1;
		int64_t bytes = 64;
		for (int i = 0; i < items.size() && bytes <= MAX_VALUE_BYTES; i++) bytes += estimate_data(items[i], depth + 1, ancestors);
		return bytes;
	}
	if (type == Variant::DICTIONARY) {
		Dictionary items = value;
		if (items.size() > MAX_VALUE_BYTES / 64) return MAX_VALUE_BYTES + 1;
		int64_t bytes = 64;
		for (const Variant &key : items.keys()) { bytes += estimate_data(key, depth + 1, ancestors) + estimate_data(items[key], depth + 1, ancestors); if (bytes > MAX_VALUE_BYTES) break; }
		return bytes;
	}
	return 64;
}

int64_t ValueCodec::estimate(const Variant &value, int depth) {
	std::vector<Variant> ancestors;
	return estimate_data(value, depth, ancestors);
}

static bool inline_graph(const Variant &value, std::vector<Variant> &ancestors, int depth = 0) {
	if (value.get_type() != Variant::ARRAY && value.get_type() != Variant::DICTIONARY) return true;
	if (depth >= ValueCodec::MAX_DEPTH) return false;
	for (const Variant &ancestor : ancestors) if (UtilityFunctions::is_same(value, ancestor)) return false;
	ancestors.push_back(value);
	struct Pop { std::vector<Variant> &values; ~Pop() { values.pop_back(); } } pop{ancestors};
	if (value.get_type() == Variant::ARRAY) {
		for (const Variant &item : Array(value)) if (!inline_graph(item, ancestors, depth + 1)) return false;
	} else {
		Dictionary items = value;
		for (const Variant &key : items.keys()) if (!inline_graph(key, ancestors, depth + 1) || !inline_graph(items[key], ancestors, depth + 1)) return false;
	}
	return true;
}

Variant ValueCodec::encode(const Variant &value, String path, int depth) {
	if (depth > MAX_DEPTH) {
		if (retain) return retain(value);
		fail("VALUE_DEPTH_LIMIT", "output nesting limit exceeded", path);
	}
	int type = value.get_type();
	if (type == Variant::INT) {
		int64_t number = value;
		if (number >= -SAFE_INTEGER && number <= SAFE_INTEGER) return value;
		Dictionary out = tagged("int"); out["value"] = String::num_int64(number); return out;
	}
	if (type == Variant::FLOAT) {
		double number = value;
		if (std::isfinite(number) && !(number == 0 && std::signbit(number))) return value;
		Dictionary out = tagged("float"); out["value"] = std::isnan(number) ? "NaN" : std::isinf(number) ? number < 0 ? "-Infinity" : "+Infinity" : "-0"; return out;
	}
	if (type <= Variant::STRING) return value;
	if (type == Variant::STRING_NAME || type == Variant::NODE_PATH) { Dictionary out = tagged(Variant::get_type_name(value.get_type())); out["value"] = String(value); return out; }
	if (type == Variant::OBJECT) {
		if (!value.get_validated_object()) return Variant();
		if (object_wire) return object_wire(value);
		fail("INVALID_HANDLE", "object encoding unavailable", path);
	}
	std::vector<ValueField> shape = fields(type);
	if (!shape.empty() && finite_math(value)) {
		Dictionary out = tagged(Variant::get_type_name(value.get_type()));
		for (const ValueField &field : shape) { bool valid = false; out[field.name] = encode(value.get_named(field.name, valid), path + "." + field.name, depth + 1); }
		return out;
	}
	int element = packed_element(type);
	if (type == Variant::ARRAY || type == Variant::DICTIONARY || element >= 0) {
		for (const Variant &ancestor : active) if (UtilityFunctions::is_same(value, ancestor)) {
			if (retain) return retain(value);
			fail("INVALID_VALUE_ENCODING", "cyclic native collection", path);
		}
		Variant sized = value;
		int64_t count = sized.call("size");
		std::vector<Variant> graph;
		if (count > INLINE_ELEMENTS || estimate(value) > INLINE_BYTES || !inline_graph(value, graph)) {
			if (retain) return retain(value);
			fail("OUTPUT_LIMIT", "collection requires bounded value reference", path);
		}
		uint64_t checkpoint = retention_checkpoint ? retention_checkpoint() : 0;
		auto bounded = [&](const Variant &encoded) -> Variant {
			if (JSON::stringify(encoded).utf8().length() <= INLINE_BYTES) return encoded;
			if (rollback_retention) rollback_retention(checkpoint);
			if (retain) return retain(value);
			fail("OUTPUT_LIMIT", "encoded collection exceeds inline byte budget", path);
		};
		auto type_info = [&](int builtin, const StringName &cls, const Variant &script) {
			return collection_type ? collection_type(builtin, cls, script) : descriptor(builtin == Variant::NIL ? String("Variant") : builtin == Variant::OBJECT ? cls.is_empty() ? String("Object") : String(cls) : Variant::get_type_name((Variant::Type)builtin));
		};
		active.push_back(value);
		struct Pop { std::vector<Variant> &values; ~Pop() { values.pop_back(); } } pop{active};
		if (type == Variant::DICTIONARY) {
			Dictionary source = value, out;
			bool entries_needed = source.is_typed() || reserved(source);
			for (const Variant &key : source.keys()) if (key.get_type() != Variant::STRING) entries_needed = true;
			Array entries;
			for (const Variant &key : source.keys()) {
				Variant encoded = encode(source[key], path + ".value", depth + 1);
				if (!entries_needed) out[key] = encoded;
				else { Dictionary entry; entry["key"] = encode(key, path + ".key", depth + 1); entry["value"] = encoded; entries.push_back(entry); }
			}
			if (entries_needed) {
				out = tagged("Dictionary"); out["entries"] = entries;
				if (source.is_typed()) {
					Dictionary key_type = type_info(source.get_typed_key_builtin(), source.get_typed_key_class_name(), source.get_typed_key_script());
					Dictionary value_type = type_info(source.get_typed_value_builtin(), source.get_typed_value_class_name(), source.get_typed_value_script());
					if (key_type.is_empty() || value_type.is_empty()) { if (rollback_retention) rollback_retention(checkpoint); if (retain) return retain(value); fail("TYPE_METADATA_UNAVAILABLE", "typed collection metadata unavailable", path); }
					out["key_type"] = key_type;
					out["value_type"] = value_type;
				}
			}
			return bounded(out);
		}
		Array out;
		for (int64_t i = 0; i < count; i++) { bool valid = false, oob = false; out.push_back(encode(value.get_indexed(i, valid, oob), path + "[" + itos(i) + "]", depth + 1)); }
		if (element >= 0) { Dictionary data = tagged(Variant::get_type_name(value.get_type())); data["values"] = out; return bounded(data); }
		Array source = value;
		if (source.is_typed()) {
			Dictionary data = tagged("Array"); data["values"] = out;
			Dictionary constraint = type_info(source.get_typed_builtin(), source.get_typed_class_name(), source.get_typed_script());
			if (constraint.is_empty()) { if (rollback_retention) rollback_retention(checkpoint); if (retain) return retain(value); fail("TYPE_METADATA_UNAVAILABLE", "typed collection metadata unavailable", path); }
			data["element_type"] = constraint;
			return bounded(data);
		}
		return bounded(out);
	}
	if (retain) return retain(value);
	fail("UNSUPPORTED_VARIANT_TYPE", "value requires an opaque reference", path, "", Variant::get_type_name(value.get_type()));
}

Dictionary ValueCodec::describe_type(const String &name) {
	Dictionary info = descriptor(name);
	int type = type_id(info);
	if (type < 0 || (type == Variant::OBJECT && name != "Object")) return Dictionary();
	info["contextual"] = type != Variant::RID && type != Variant::OBJECT && type != Variant::CALLABLE && type != Variant::SIGNAL;
	info["opaque_only"] = type == Variant::RID || type == Variant::CALLABLE || type == Variant::SIGNAL;
	info["category"] = type == Variant::OBJECT ? "object" : (bool)info["opaque_only"] ? "opaque" : !fields(type).empty() ? "named" : packed_element(type) >= 0 ? "packed" : type == Variant::ARRAY || type == Variant::DICTIONARY ? "collection" : type == Variant::NODE_PATH || type == Variant::STRING_NAME ? "string_like" : "scalar";
	Array components;
	for (const ValueField &field : fields(type)) { Dictionary item = descriptor(Variant::get_type_name(field.type)); item["name"] = field.name; item["required"] = !field.optional; if (field.optional) item["default"] = 1.0; components.push_back(item); }
	info["fields"] = components;
	info["positional_components"] = false;
	info["max_depth"] = MAX_DEPTH;
	info["inline_elements"] = INLINE_ELEMENTS;
	info["inline_bytes"] = INLINE_BYTES;
	info["max_value_references"] = MAX_VALUE_REFERENCES;
	info["max_value_bytes"] = MAX_VALUE_BYTES;
	info["explicit_tag"] = type == Variant::OBJECT || (bool)info["opaque_only"] ? Variant() : Variant(name);
	info["notes"] = "Named conversion requires a declared destination; Variant/untyped objects stay dictionaries. Object/value references are different. No units, normalization, clamping or capability construction is inferred.";
	String category = info["category"];
	if (category == "named") {
		info["contextual_format"] = "Exact named object with the listed fields; nested fields use their declared type. No positional arrays.";
		info["explicit_format"] = "{$t:'" + name + "',...namedFields}";
		info["output_format"] = "Tagged named fields, or an opaque value reference for non-JSON mathematical components.";
		info["finite_components"] = true;
		info["component_bits"] = type == Variant::COLOR || type == Variant::VECTOR2I || type == Variant::VECTOR3I || type == Variant::VECTOR4I ? 32 : int(sizeof(real_t) * 8);
	} else if (category == "opaque" || category == "object") {
		info["contextual_format"] = category == "object" ? "Validated object handle, or null; client class labels do not grant compatibility." : "A legitimate server-issued value reference of the required actual type.";
		info["explicit_format"] = Variant();
		info["output_format"] = category == "object" ? "{__godotHandle:string,class_name?:string,script_class?:string}" : "{__godotValue:string,type:string}";
		info["reference_operations"] = category == "object" ? "Authorized godot.get/set/call; not godot.values." : "await godot.values.inspect(ref); read is UNSUPPORTED_VALUE_OPERATION for opaque-only capabilities; release requires authorized untracked.";
	} else if (category == "packed" || category == "collection") {
		int element = packed_element(type);
		if (element >= 0) info["element_type"] = descriptor(Variant::get_type_name((Variant::Type)element));
		info["contextual_format"] = type == Variant::DICTIONARY ? "Plain JSON object for ordinary string keys; use entries for non-string or reserved keys." : "Array of fully validated elements; mathematical elements use named objects, not positional arrays.";
		info["explicit_format"] = type == Variant::DICTIONARY ? "{$t:'Dictionary',entries:[{key,value}],key_type?:{type:string},value_type?:{type:string}}" : "{$t:'" + name + "',values:[...],element_type?:{type:string}}";
		if (element >= 0) info["explicit_format"] = "{$t:'" + name + "',values:[...]}";
		info["output_format"] = "Small collections are inline (Packed/typed/reserved-key collections are tagged); large, deep or cyclic collections use {__godotValue:string,type:string}.";
		info["reference_operations"] = "await godot.values.inspect(ref); await godot.values.read(ref,{offset:0,limit:64}); limit 1..256, page <=16384 bytes. Release requires authorized untracked.";
	} else {
		info["contextual_format"] = category == "string_like" || type == Variant::STRING ? "string; no coercion or path resolution" : type == Variant::NIL ? "null" : type == Variant::BOOL ? "boolean" : type == Variant::INT ? "Safe integral Number, signed-64 BigInt or canonical decimal int tag" : "number or explicit float tag; no strings/booleans coerced";
		info["explicit_format"] = "{$t:'" + name + "',value:...}";
		info["output_format"] = category == "string_like" ? "{$t:'" + name + "',value:string}" : type == Variant::INT ? "Safe Number or {$t:'int',value:decimalString}" : type == Variant::FLOAT ? "Finite number or {$t:'float',value:'NaN'|'+Infinity'|'-Infinity'|'-0'}" : "JSON primitive";
	}
	if (type == Variant::INT || type == Variant::PACKED_INT64_ARRAY) { info["integer_min"] = "-9223372036854775808"; info["integer_max"] = "9223372036854775807"; info["safe_number_max"] = SAFE_INTEGER; }
	if (type == Variant::PACKED_BYTE_ARRAY) { info["integer_min"] = "0"; info["integer_max"] = "255"; }
	if (type == Variant::PACKED_INT32_ARRAY || type == Variant::VECTOR2I || type == Variant::VECTOR3I || type == Variant::VECTOR4I) { info["integer_min"] = "-2147483648"; info["integer_max"] = "2147483647"; }
	if (type == Variant::PACKED_FLOAT32_ARRAY) info["float_rules"] = "IEEE float32 rounding is allowed; finite overflow is rejected. Explicit float tags preserve exceptional values.";
	return info;
}

Array ValueCodec::types() {
	Array result;
	for (int i = 0; i < Variant::VARIANT_MAX; i++) result.push_back(describe_type(Variant::get_type_name((Variant::Type)i)));
	return result;
}

}
