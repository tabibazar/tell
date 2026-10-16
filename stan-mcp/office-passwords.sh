#!/bin/sh
# The HOA office console's password for each community (derived from the
# server token in the git-ignored secrets/stan-mcp.env; never in git).
#   sh stan-mcp/office-passwords.sh
cd "$(dirname "$0")/.."
set -a; . ./secrets/stan-mcp.env; set +a
python3 - <<'PY'
import os, sys, urllib.parse
sys.path.insert(0, "stan-mcp")
import office
u = urllib.parse.urlparse(os.environ["MCP_URL"])
print(f"console: {u.scheme}://{u.netloc}/office")
for c in ("sheppard center", "stan meadows", "harbor view", "oak ridge"):
    print(f"  {c:16} {office.office_password(c)}")
PY
