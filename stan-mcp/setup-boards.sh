#!/bin/sh
# The boards' dashboard: put BOARDS_TOKEN (from the gitignored secrets/boards.env)
# into the Lambda's environment beside what is already there, then the code.
# Prints no secret.   sh stan-mcp/setup-boards.sh
set -e
cd "$(dirname "$0")/.."
FN=stan-mcp-demo
R=us-east-1
. secrets/boards.env
aws lambda get-function-configuration --function-name $FN --region $R --query Environment > /tmp/stanmcp-env.json
BOARDS_TOKEN="$BOARDS_TOKEN" python3 - <<'PY'
import json, os
p = "/tmp/stanmcp-env.json"
e = json.load(open(p)) or {}
e.setdefault("Variables", {})["BOARDS_TOKEN"] = os.environ["BOARDS_TOKEN"]
json.dump(e, open(p, "w"))
PY
aws lambda update-function-configuration --function-name $FN --region $R --environment file:///tmp/stanmcp-env.json >/dev/null
rm -f /tmp/stanmcp-env.json
aws lambda wait function-updated --function-name $FN --region $R
(cd stan-mcp && rm -f /tmp/stanmcp.zip && zip -qr /tmp/stanmcp.zip lambda_function.py hoa_data.py family.py state.py mail.py library.py office.py civic.py boards.py library)
aws lambda update-function-code --function-name $FN --region $R --zip-file fileb:///tmp/stanmcp.zip >/dev/null
aws lambda wait function-updated --function-name $FN --region $R
echo "dashboard: ${BOARDS_URL%/report}"
