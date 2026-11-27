def test_widget_setup(session):
    session.ensure("widget_setup")


def test_group_saves(session):
    session.ensure("group_saves")


def test_variant_matrix(session):
    session.ensure("variant_matrix")


def test_diagnostics(session):
    session.ensure("diagnostics")


def test_rotation_contract(session):
    session.ensure("rotation_contract")


def test_collections(session):
    session.ensure("collections")


def test_value_references(session):
    session.ensure("value_references")


def test_page_boundaries(session):
    session.ensure("page_boundaries")


def test_checkpoints(session):
    session.ensure("checkpoints")


def test_reference_quotas(session):
    session.ensure("reference_quotas")


def test_integration_contracts(session):
    session.ensure("integration_contracts")


def test_widget_contract(session):
    session.ensure("widget_contract")
