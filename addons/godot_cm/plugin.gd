@tool
extends EditorPlugin

const DEFAULT_PORT := 8765
const MAX_PORT_TRIES := 10

var _runner: RefCounted
var _service: RefCounted
var _dock: HBoxContainer
var _status: Label

# Catppuccin Mocha palette
const MOCHA := {
	"green": Color(0.651, 0.890, 0.631),    # a6e3a1
	"red": Color(0.953, 0.545, 0.659),      # f38ba8
	"yellow": Color(0.976, 0.886, 0.686),   # f9e2af
	"blue": Color(0.537, 0.706, 0.980),     # 89b4fa
	"mauve": Color(0.796, 0.651, 0.969),    # cba6f7
	"text": Color(0.804, 0.839, 0.957),     # cdd6f4
	"subtext": Color(0.651, 0.678, 0.784),  # a6adc8
}


func _enter_tree() -> void:
	_runner = SpikeRunner.new()
	_runner.run_checks()
	_service = McpService.new()

	var base := int(OS.get_environment("GODOT_CM_PORT")) if OS.has_environment("GODOT_CM_PORT") else DEFAULT_PORT
	if base <= 0 or base > 65535:
		base = DEFAULT_PORT

	var port := base
	while port <= base + MAX_PORT_TRIES:
		if _service.start(port):
			break
		push_warning("[godot-cm] port %d is busy, trying %d" % [port, port + 1])
		port += 1

	if _service.is_running():
		var msg := "godot-cm MCP server on 127.0.0.1:%d" % port
		if port != base:
			msg += " (default %d was busy)" % base
		print_rich("[color=green][b]%s[/b][/color]" % msg)
		_service.tier_request.connect(_on_tier_request)
		_build_dock(port, base)
	else:
		var msg := "godot-cm could not bind any port in %d-%d — MCP disabled. Free a port or set GODOT_CM_PORT." % [base, base + MAX_PORT_TRIES]
		push_error(msg)
		_build_dock(0, base)


func _exit_tree() -> void:
	if _dock != null:
		remove_control_from_bottom_panel(_dock)
		_dock.queue_free()
		_dock = null
	if _service != null:
		_service.stop()
		_service = null
	_runner = null


func _build_dock(port: int, base: int) -> void:
	_dock = HBoxContainer.new()
	_dock.name = "Godot CM"
	_dock.alignment = BoxContainer.ALIGNMENT_CENTER

	var info := VBoxContainer.new()
	info.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	info.add_theme_constant_override("separation", 8)
	_dock.add_child(info)

	# --- Status + endpoint ---
	var header := HBoxContainer.new()
	header.add_theme_constant_override("separation", 12)
	info.add_child(header)

	var dot := Label.new()
	dot.add_theme_font_size_override("font_size", 18)
	header.add_child(dot)

	_status = Label.new()
	_status.add_theme_font_size_override("font_size", 16)
	header.add_child(_status)

	var url_label := Label.new()
	url_label.add_theme_font_size_override("font_size", 22)
	url_label.add_theme_color_override("font_color", MOCHA.blue)
	header.add_child(url_label)

	if port > 0:
		dot.text = "●"
		dot.add_theme_color_override("font_color", MOCHA.green)
		_status.text = "MCP server running"
		_status.add_theme_color_override("font_color", MOCHA.text)
		var url := "http://127.0.0.1:%d/mcp" % port
		url_label.text = url
		url_label.tooltip_text = "MCP endpoint — point your client here"
		var copy_url := Button.new()
		copy_url.text = "Copy URL"
		copy_url.tooltip_text = "Copy the full MCP endpoint URL to the clipboard"
		copy_url.pressed.connect(func() -> void:
			DisplayServer.clipboard_set(url)
			copy_url.text = "Copied!"
		)
		header.add_child(copy_url)
	else:
		dot.text = "●"
		dot.add_theme_color_override("font_color", MOCHA.red)
		_status.text = "MCP server FAILED — no free port in %d-%d. Set GODOT_CM_PORT." % [base, base + MAX_PORT_TRIES]
		_status.add_theme_color_override("font_color", MOCHA.red)

	if port > 0 and port != base:
		var note := Label.new()
		note.text = "Default %d was busy — point your client at this port" % base
		note.add_theme_color_override("font_color", MOCHA.yellow)
		info.add_child(note)

	if port > 0:
		# --- Session token ---
		var token_row := HBoxContainer.new()
		token_row.add_theme_constant_override("separation", 8)
		info.add_child(token_row)

		var token_label := Label.new()
		token_label.text = "Session token:"
		token_label.add_theme_color_override("font_color", MOCHA.subtext)
		token_row.add_child(token_label)

		var token_edit := LineEdit.new()
		token_edit.text = _service.get_session_token()
		token_edit.editable = false
		token_edit.selecting_enabled = true
		token_edit.custom_minimum_size.x = 300
		token_edit.tooltip_text = "Clients send this as the MCP-Session-Id header. Also saved to user://godot_cm_session"
		token_row.add_child(token_edit)

		var copy_btn := Button.new()
		copy_btn.text = "Copy token"
		copy_btn.tooltip_text = "Copy the session token to the clipboard"
		copy_btn.pressed.connect(func() -> void:
			DisplayServer.clipboard_set(_service.get_session_token())
			copy_btn.text = "Copied!"
		)
		token_row.add_child(copy_btn)

		info.add_child(HSeparator.new())

		# --- Permissions ---
		var perm_header := Label.new()
		perm_header.text = "Agent permissions"
		perm_header.add_theme_color_override("font_color", MOCHA.mauve)
		info.add_child(perm_header)

		var grant_box := CheckBox.new()
		grant_box.text = "Auto-approve permission tiers for this session"
		grant_box.tooltip_text = "When checked, godot.requestTier() is granted instantly with no dialog — trust mode for agents you control."
		grant_box.toggled.connect(func(on: bool) -> void:
			_service.set_auto_grant(on)
		)
		info.add_child(grant_box)

		var policies := HBoxContainer.new()
		policies.add_theme_constant_override("separation", 8)
		info.add_child(policies)
		var policies_label := Label.new()
		policies_label.text = "Tier policy:"
		policies_label.add_theme_color_override("font_color", MOCHA.subtext)
		policies_label.tooltip_text = "Per-tier: allow = granted instantly, ask = confirmation dialog, deny = always refused.\nTightening a policy relocks the tier immediately; loosening unlocks it."
		policies.add_child(policies_label)
		const POLICIES := ["allow", "ask", "deny"]
		const TIER_BLURBS := {
			"network": "Sockets, HTTP requests, multiplayer peers (ENet, WebSocket, WebRTC).\nLow risk on its own — the agent can't reach your files without the filesystem tier.",
			"filesystem": "Reading/writing arbitrary files on disk (FileAccess, DirAccess, ResourceLoader…).\nThis is the sensitive one: it can read .env files, SSH keys, project secrets.\nKeeping it on 'ask' means you get a dialog before any file access.",
		}
		for tier in _service.get_tiers():
			var classes: Array = _service.get_tier_classes(tier)
			var examples := ", ".join(classes.slice(0, 8))
			if classes.size() > 8:
				examples += ", …"
			var blurb: String = TIER_BLURBS.get(tier, "")
			var name_lbl := Label.new()
			name_lbl.text = " %s " % tier
			name_lbl.add_theme_color_override("font_color", MOCHA.text)
			name_lbl.tooltip_text = "\"%s\" tier — %d engine classes gated until unlocked.\n\n%s\n\nExamples: %s" % [tier, classes.size(), blurb, examples]
			policies.add_child(name_lbl)
			var combo := OptionButton.new()
			for p in POLICIES:
				combo.add_item(p)
			combo.selected = max(0, POLICIES.find(_service.get_tier_policy(tier)))
			combo.tooltip_text = "%s\n\nallow: godot.requestTier(\"%s\") is granted instantly\nask: a dialog asks you each time the agent requests it\ndeny: requests are refused with no dialog" % [blurb, tier]
			var t: String = tier
			var svc := _service
			combo.item_selected.connect(func(idx: int) -> void:
				svc.set_tier_policy(t, combo.get_item_text(idx))
			)
			policies.add_child(combo)

	add_control_to_bottom_panel(_dock, "Godot CM")
	# Show it once on enable so the user notices the address/token —
	# deferred: the bottom panel isn't ready during _enter_tree.
	_reveal.call_deferred()


func _reveal() -> void:
	if _dock != null:
		make_bottom_panel_item_visible(_dock)


func _on_tier_request(tier: String, classes: Array) -> void:
	# The agent is blocked on godot.requestTier() until the user answers.
	var dlg := ConfirmationDialog.new()
	dlg.title = "Godot CM — permission request"
	var preview := ", ".join(classes.slice(0, 6))
	if classes.size() > 6:
		preview += ", …"
	dlg.dialog_text = "The agent requests the \"%s\" permission tier:\n\n%s\n\nAllow for this session?" % [tier, preview]
	dlg.ok_button_text = "Allow"
	dlg.cancel_button_text = "Deny"
	var svc := _service
	dlg.confirmed.connect(func() -> void:
		svc.respond_tier_request(tier, true)
		dlg.queue_free()
	)
	dlg.canceled.connect(func() -> void:
		svc.respond_tier_request(tier, false)
		dlg.queue_free()
	)
	get_editor_interface().get_base_control().add_child(dlg)
	dlg.popup_centered()
