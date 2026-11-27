def test_discovery(session):
    session.ensure("discovery")


def test_node_ops(session):
    session.ensure("node_ops")


def test_tier_gate(session):
    session.ensure("tier_gate")


def test_tool_contract(session):
    session.ensure("tool_contract")


def test_permissions(session):
    session.ensure("permissions")


def test_policy_reload(session):
    session.ensure("policy_reload")


def test_script_ops(session):
    session.ensure("script_ops")
