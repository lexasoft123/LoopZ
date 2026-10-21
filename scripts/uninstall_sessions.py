"""Read-only admission for every active launchd GUI session.

No account enumeration, user job shutdown, or login registration changes occur
here. Logged-out accounts remain a separate uninstall acceptance requirement.
"""

import copy
import re

LABELS = (
    "com.lexasoft.loopz.engine",
    "com.lexasoft.loopz.session",
    "com.lexasoft.loopz.login",
)
MAX_OUTPUT = 8 * 1024 * 1024
MAX_DOMAINS = 8192
MAX_USERS = 256


def lines_for(response, domain):
    if response.returncode or len(response.stdout) > MAX_OUTPUT:
        raise RuntimeError(
            f"Cannot inspect launchd domain {domain}; administrator preflight may be required"
        )
    lines = response.stdout.splitlines()
    if not lines or lines[0] != f"{domain} = {{" or lines[-1] != "}":
        raise RuntimeError("Unrecognized launchd domain format")
    return lines


def block(lines, name):
    header = f"\t{name} = {{"
    if lines.count(header) != 1:
        raise RuntimeError(f"Missing or ambiguous launchd {name}")
    start = lines.index(header) + 1
    try:
        end = lines.index("\t}", start)
    except ValueError as error:
        raise RuntimeError(f"Incomplete launchd {name}") from error
    return lines[start:end]


def field(lines, name, indent="\t"):
    values = [
        line[len(indent + name + " = ") :]
        for line in lines
        if line.startswith(indent + name + " = ")
    ]
    if len(values) != 1:
        raise RuntimeError(f"Missing or ambiguous launchd {name}")
    return values[0]


def number(value):
    if not re.fullmatch(r"[0-9]{1,10}", value) or int(value) > 0xFFFFFFFF:
        raise RuntimeError("Invalid launchd numeric identity")
    return int(value)


class SessionInspector:
    def __init__(self, command, login_for_uid):
        self.command = command
        self.login_for_uid = login_for_uid

    def discover(self):
        lines = lines_for(self.command("/bin/launchctl", "print", "system", check=False), "system")
        if field(lines, "type") != "system" or field(lines, "handle") != "0":
            raise RuntimeError("Unexpected launchd system identity")
        children = block(lines, "subdomains")
        if len(children) > MAX_DOMAINS:
            raise RuntimeError("Too many launchd subdomains for bounded session review")
        users = set()
        for child in children:
            match = re.fullmatch(r"\t\t(pid|user|gui|login|session)/([0-9]{1,10})", child)
            if not match:
                raise RuntimeError("Unrecognized launchd subdomain")
            identity = number(match[2])
            if match[1] in ("user", "gui"):
                users.add(identity)
            elif match[1] in ("login", "session"):
                raise RuntimeError(
                    "Unmapped launchd login domain requires administrator inspection"
                )
        if len(users) > MAX_USERS:
            raise RuntimeError("Too many user domains for bounded session review")
        result = []
        for uid in sorted(users):
            domain = f"gui/{uid}"
            response = self.command("/bin/launchctl", "print", domain, check=False)
            # launchd returns ENOTSUP for a user domain with no GUI alias.
            # Permission errors, vanished domains, and unknown errors fail closed.
            if response.returncode == 125 and (response.stdout + response.stderr).strip() == (
                "Could not print domain: 125: Domain does not support specified action"
            ):
                continue
            lines = lines_for(response, domain)
            security = block(lines, "security context")
            identity = {
                "uid": number(field(security, "uid", "\t\t")),
                "asid": number(field(security, "asid", "\t\t")),
                "handle": number(field(lines, "handle")),
            }
            if (
                field(lines, "type") != "login"
                or identity["uid"] != uid
                or not all(identity.values())
            ):
                raise RuntimeError("Unexpected launchd GUI identity")
            result.append(identity)
        return result

    def snapshot(self, console_uid, desktop_available):
        identities = self.discover()
        if console_uid not in {item["uid"] for item in identities}:
            raise RuntimeError("Console user is missing from the GUI session review")
        accounts = []
        for identity in identities:
            jobs = {}
            for label in LABELS:
                response = self.command(
                    "/bin/launchctl", "print", f"gui/{identity['uid']}/{label}", check=False
                )
                if response.returncode == 0:
                    jobs[label] = True
                elif (
                    response.returncode in (3, 113)
                    and "Could not find service" in response.stdout + response.stderr
                ):
                    jobs[label] = False
                else:
                    raise RuntimeError("A signed-in user's LoopZ jobs could not be inspected")
            login = self.login_for_uid(identity["uid"]) if desktop_available else None
            accounts.append({**identity, "jobs": jobs, "login": login})
        if identities != self.discover():
            raise RuntimeError("GUI sessions changed during removal review; retry preflight")
        return {
            "version": 1,
            "coverage": "activeGuiSessions",
            "desktopAvailable": desktop_available,
            "accounts": accounts,
        }


def validate_sessions(state):
    if (
        not isinstance(state, dict)
        or type(state.get("version")) is not int
        or state["version"] != 1
        or state.get("coverage") != "activeGuiSessions"
        or type(state.get("desktopAvailable")) is not bool
    ):
        raise ValueError("Missing complete active GUI session review")
    accounts = state.get("accounts")
    if not isinstance(accounts, list) or not 1 <= len(accounts) <= MAX_USERS:
        raise ValueError("Invalid GUI session count")
    uids = set()
    for account in accounts:
        if not isinstance(account, dict):
            raise ValueError("Invalid GUI session record")
        for key in ("uid", "asid", "handle"):
            if type(account.get(key)) is not int or not 0 < account[key] <= 0xFFFFFFFF:
                raise ValueError("Invalid GUI session identity")
        if account["uid"] in uids:
            raise ValueError("Duplicate GUI session user")
        uids.add(account["uid"])
        jobs = account.get("jobs")
        if (
            not isinstance(jobs, dict)
            or set(jobs) != set(LABELS)
            or any(type(v) is not bool for v in jobs.values())
        ):
            raise ValueError("Incomplete GUI job review")
        login = account.get("login")
        if login is None:
            if state["desktopAvailable"]:
                raise ValueError("Installed desktop did not report login state")
        elif (
            not state["desktopAvailable"]
            or not isinstance(login, dict)
            or any(type(login.get(k)) is not bool for k in ("registered", "requiresApproval"))
        ):
            raise ValueError("Incomplete per-user login review")
    return accounts


def require_sessions_off(state, recorded=None, *, allow_new_generation=False):
    accounts = validate_sessions(state)
    previous = {a["uid"]: a for a in validate_sessions(recorded)} if recorded is not None else {}
    for account in accounts:
        uid = account["uid"]
        if any(account["jobs"].values()):
            raise RuntimeError(f"Stop LoopZ jobs in signed-in account {uid} before removal")
        login = account["login"]
        if login is None:
            original = previous.get(uid)
            if (
                original is None
                or not allow_new_generation
                and any(account[k] != original[k] for k in ("asid", "handle"))
            ):
                raise RuntimeError(
                    f"No verified login choice for signed-in account {uid}; restore the desktop before retrying"
                )
            login = original["login"]
        if (
            not isinstance(login, dict)
            or login.get("registered") is not False
            or login.get("requiresApproval") is not False
        ):
            raise RuntimeError(
                f"Turn off LoopZ Start at login in signed-in account {uid} before removal"
            )
    return copy.deepcopy(state)


def validate_session_preservation(before, after):
    old = validate_sessions(before)
    new = validate_sessions(after)
    identities = lambda accounts: sorted((a["uid"], a["asid"], a["handle"]) for a in accounts)
    if identities(old) != identities(new):
        raise RuntimeError("GUI sessions changed during the removal transaction")
    for account in new:
        if (
            any(account["jobs"].values())
            or account["login"] is not None
            and (
                account["login"].get("registered") is not False
                or account["login"].get("requiresApproval") is not False
            )
        ):
            raise RuntimeError("LoopZ session or login state changed during removal")
