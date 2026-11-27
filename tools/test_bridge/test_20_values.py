def test_passive_data(session):
    session.ensure("passive_data")


def test_value_validation(session):
    session.ensure("value_validation")


def test_effects_rollback(session):
    session.ensure("effects_rollback")
