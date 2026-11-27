def test_loader(session):
    session.ensure("loader")


def test_catalog_freshness(session):
    session.ensure("catalog_freshness")


def test_tail(session):
    session.ensure("tail")
