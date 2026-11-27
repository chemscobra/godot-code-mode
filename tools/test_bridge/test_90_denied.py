def test_discovery(denied_session):
    denied_session.ensure("discovery")


def test_node_ops(denied_session):
    denied_session.ensure("node_ops")


def test_tier_gate_denied(denied_session):
    denied_session.ensure("tier_gate")
    assert denied_session.blocked
