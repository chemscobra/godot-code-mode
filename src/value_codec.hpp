#ifndef GODOT_CM_VALUE_CODEC_HPP
#define GODOT_CM_VALUE_CODEC_HPP

#include <godot_cpp/variant/variant.hpp>
#include <exception>
#include <functional>
#include <vector>

namespace godot_cm {

struct ValueFailure : std::exception {
	godot::Dictionary diagnostic;
	explicit ValueFailure(const godot::Dictionary &value) : diagnostic(value) {}
	const char *what() const noexcept override { return "Value conversion failed"; }
};

struct ValueField {
	const char *name;
	godot::Variant::Type type;
	bool optional = false;
};

class ValueCodec {
public:
	static constexpr int MAX_DEPTH = 32;
	static constexpr int INLINE_ELEMENTS = 256;
	static constexpr int INLINE_BYTES = 16 << 10;
	static constexpr int MAX_VALUE_REFERENCES = 4096;
	static constexpr int64_t MAX_VALUE_BYTES = 64ll << 20;
	static constexpr int64_t SAFE_INTEGER = 9007199254740991ll;

	std::function<godot::Variant(const godot::Dictionary &, const godot::String &)> resolve;
	std::function<godot::Variant(const godot::Variant &)> retain;
	std::function<godot::Variant(const godot::Variant &)> object_wire;
	std::function<bool(godot::Object *, const godot::String &)> object_matches;
	std::function<godot::Dictionary(const godot::Dictionary &)> storage_type;
	std::function<godot::Dictionary(int, const godot::StringName &, const godot::Variant &)> collection_type;
	std::function<uint64_t()> retention_checkpoint;
	std::function<void(uint64_t)> rollback_retention;

	static int type_id(const godot::Dictionary &descriptor);
	static godot::Dictionary descriptor(const godot::String &name);
	static godot::Dictionary describe_type(const godot::String &name);
	static godot::Array types();
	static std::vector<ValueField> fields(int type);
	[[noreturn]] static void fail(const godot::String &code, const godot::String &message, godot::String path = "value", const godot::String &expected = "", const godot::String &received = "");
	static int64_t integer_text(const godot::String &text, godot::String path);
	static int64_t estimate(const godot::Variant &value, int depth = 0);

	godot::Variant decode(const godot::Variant &wire, const godot::Dictionary &expected = godot::Dictionary(), godot::String path = "value", int depth = 0);
	godot::Variant encode(const godot::Variant &value, godot::String path = "result", int depth = 0);
	godot::Dictionary native_descriptor(const godot::Variant &value) const;

private:
	std::vector<godot::Variant> active;
	int64_t visited = 0;
	godot::Variant native_checked(const godot::Variant &value, const godot::Dictionary &expected, godot::String path, int depth);
	godot::Dictionary collection_constraint(const godot::Dictionary &destination, const godot::Dictionary &explicit_info, const char *key, godot::String path);
};

}

#endif
