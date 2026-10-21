"""App ownership policy; privileged backend components keep the system owner."""

APPLICATIONS = frozenset({"LoopZ.app", "LoopZ Engine.app"})
REMOVAL_APPLICATIONS = frozenset({"desktop", "engine"})


def application_owners(uid, gid):
    if type(uid) is not int or uid < 501 or type(gid) is not int or gid < 0:
        raise ValueError("Applications require a regular console account")
    return {name: (uid, gid) for name in APPLICATIONS}


def snapshot_owner(component, node, system_owner, user_components):
    """Only explicit app roles may use a regular account instead of root.

    An entire component must retain this single owner. A nested file cannot
    widen the policy, and journals/quarantines always keep their system owner.
    """
    owner = node.get("owner") if isinstance(node, dict) else None
    if system_owner == 0 and component in user_components and type(owner) is int and owner >= 501:
        return owner
    return system_owner
