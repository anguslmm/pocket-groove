# shellcheck shell=bash
# tools/sandbox-pio-setup.sh -- make `pio run -e cardputer-adv` work in the cloud sandbox.
#
#   source tools/sandbox-pio-setup.sh && pio run -e cardputer-adv
#
# Idempotent; safe to source in every fresh shell (shell env does not persist
# between tool calls). First build on a clean machine downloads ~1 GB of
# packages from github.com release assets and takes a few minutes.
#
# NOT needed on a normal Mac/PC: there `pip install platformio` (or the VS Code
# extension) + `pio run -e cardputer-adv` works with the same platformio.ini.
#
# Why each step exists (all are sandbox-only accommodations):
#
#  1. PlatformIO core = `pioarduino==6.2.0` in ~/.platformio/penv, first on PATH.
#     Upstream `platformio` fetches its build engine (tool-scons) from
#     registry.platformio.org, which the sandbox proxy denies (403). The
#     pioarduino fork of the core -- the one the pioarduino platform installs
#     into this very penv anyway -- fetches scons from github.com instead.
#     Pinned to 6.2.0 because 6.2.0.post1 is read as a pre-release
#     ("6.2.0-post.1") and then rejects (and uninstalls!) the platform, whose
#     manifest asks for core >=6.2.0.
#
#  2. CA trust. Sandbox egress is TLS-inspected; the CAs to trust are in
#     /root/.ccr/ca-bundle.crt. The pioarduino platform overwrites
#     SSL_CERT_FILE / REQUESTS_CA_BUNDLE / CURL_CA_BUNDLE / GIT_SSL_CAINFO with
#     the penv's own certifi bundle on every run, so env vars alone cannot
#     work. We append the sandbox CAs to that certifi bundle (verification
#     stays ON). Re-applied on each source in case certifi was reinstalled.
#
#  3. Telemetry off: collector.platformio.org is denied by the proxy anyway.
#
# Hosts the build needs (all allowed today): github.com,
# release-assets.githubusercontent.com / objects.githubusercontent.com,
# pypi.org, files.pythonhosted.org.
# Still blocked and deliberately NOT worked around: registry.platformio.org,
# api.registry.platformio.org, dl.registry.platformio.org, dl.platformio.org.
# Consequence: every lib_deps entry (and every dependency a library declares in
# its library.json) must be a git/https URL listed BEFORE the library that
# needs it -- registry names like "m5stack/M5Unified" cannot be resolved here.

_pg_pio_setup() {
    local core_ver="6.2.0"
    local core_dir="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
    local penv="$core_dir/penv"
    local ca_src="${PG_SANDBOX_CA_BUNDLE:-/root/.ccr/ca-bundle.crt}"
    local have

    command -v python3 >/dev/null 2>&1 || { echo "sandbox-pio-setup: python3 not found" >&2; return 1; }

    # Several agents share this container and ~/.platformio; serialise the
    # bootstrap so two shells sourcing this at once cannot corrupt the penv.
    # (The lock is released when this function returns.)
    local lock_fd=""
    if command -v flock >/dev/null 2>&1 && mkdir -p "$core_dir"; then
        exec {lock_fd}>"$core_dir/.sandbox-pio-setup.lock" && flock -w 300 "$lock_fd" || lock_fd=""
    fi
    _pg_pio_unlock() { [ -n "$lock_fd" ] && exec {lock_fd}>&-; return 0; }

    # -- 1. pioarduino core in the penv ------------------------------------
    if [ ! -x "$penv/bin/python" ]; then
        echo "sandbox-pio-setup: creating $penv"
        mkdir -p "$core_dir" && python3 -m venv "$penv" || { _pg_pio_unlock; return 1; }
    fi
    have="$("$penv/bin/python" -c 'import platformio; print(platformio.__version__)' 2>/dev/null)"
    if [ "$have" != "$core_ver" ] || [ ! -x "$penv/bin/pio" ]; then
        echo "sandbox-pio-setup: installing pioarduino==$core_ver into penv (had: ${have:-none})"
        if [ -x "$penv/bin/uv" ]; then
            "$penv/bin/uv" pip install --quiet --python "$penv/bin/python" \
                "pioarduino==$core_ver" certifi || { _pg_pio_unlock; return 1; }
        else
            "$penv/bin/python" -m ensurepip --default-pip >/dev/null 2>&1
            "$penv/bin/python" -m pip install --quiet --disable-pip-version-check \
                "pioarduino==$core_ver" certifi || { _pg_pio_unlock; return 1; }
        fi
    fi

    # -- 2. trust the sandbox CAs in the penv's certifi bundle --------------
    if [ -r "$ca_src" ]; then
        "$penv/bin/python" -I - "$ca_src" <<'PYEOF' || { _pg_pio_unlock; return 1; }
import re, sys
import certifi

PEM = re.compile(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", re.S)
norm = lambda pem: "".join(pem.split())

src = open(sys.argv[1], encoding="utf-8", errors="replace").read()
dst_path = certifi.where()
dst = open(dst_path, encoding="utf-8", errors="replace").read()

known = {norm(c) for c in PEM.findall(dst)}
missing = []
for cert in PEM.findall(src):
    if norm(cert) not in known:
        known.add(norm(cert))
        missing.append(cert)
if missing:
    with open(dst_path, "a", encoding="utf-8") as fh:
        fh.write("\n# --- added by tools/sandbox-pio-setup.sh from %s ---\n" % sys.argv[1])
        for cert in missing:
            fh.write(cert + "\n")
    print("sandbox-pio-setup: added %d sandbox CA cert(s) to %s" % (len(missing), dst_path))
PYEOF
    fi

    _pg_pio_unlock

    # -- 3. environment ----------------------------------------------------
    case ":$PATH:" in
        *":$penv/bin:"*) ;;
        *) export PATH="$penv/bin:$PATH" ;;
    esac
    hash -r 2>/dev/null
    export PLATFORMIO_SETTING_ENABLE_TELEMETRY=no

    echo "sandbox-pio-setup: ready -- $(command -v pio) ($(pio --version 2>&1))"
}

_pg_pio_setup
_pg_pio_setup_rc=$?
unset -f _pg_pio_setup _pg_pio_unlock
# Works both sourced and executed; when sourced, hand the status back to `&&`.
(return 0 2>/dev/null) && return "$_pg_pio_setup_rc"
exit "$_pg_pio_setup_rc"
