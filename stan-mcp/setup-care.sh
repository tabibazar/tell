#!/bin/sh
# One-time AWS setup for stan-mcp's family features, then a code deploy.
# Creates the state table, lets the Lambda's role use it and send mail as
# stan-claw@avriz.io, sets the function's environment, and schedules the
# Sunday 10 am (Eastern) family summary. Reads the token and the family
# address from the gitignored secrets/stan-mcp.env; prints neither.
#
#   sh stan-mcp/setup-care.sh            # from the repo root
set -e
cd "$(dirname "$0")/.."
R=us-east-1
FN=stan-mcp-demo
ROLE=stan-mcp-demo-lambda
TABLE=stan-mcp-demo-state
ACCT=$(aws sts get-caller-identity --query Account --output text)
. ./secrets/stan-mcp.env
: "${FAMILY_EMAIL:?add FAMILY_EMAIL=... to secrets/stan-mcp.env}"

if ! aws dynamodb describe-table --table-name $TABLE --region $R >/dev/null 2>&1; then
  aws dynamodb create-table --table-name $TABLE --region $R --billing-mode PAY_PER_REQUEST \
    --attribute-definitions AttributeName=pk,AttributeType=S AttributeName=sk,AttributeType=S \
    --key-schema AttributeName=pk,KeyType=HASH AttributeName=sk,KeyType=RANGE >/dev/null
  aws dynamodb wait table-exists --table-name $TABLE --region $R
  aws dynamodb update-time-to-live --table-name $TABLE --region $R \
    --time-to-live-specification Enabled=true,AttributeName=ttl >/dev/null
fi
echo "table: $TABLE"

aws iam put-role-policy --role-name $ROLE --policy-name stan-mcp-demo-state-and-mail --policy-document "{
 \"Version\":\"2012-10-17\",\"Statement\":[
  {\"Effect\":\"Allow\",\"Action\":[\"dynamodb:GetItem\",\"dynamodb:PutItem\",\"dynamodb:DeleteItem\",\"dynamodb:Query\"],
   \"Resource\":\"arn:aws:dynamodb:$R:$ACCT:table/$TABLE\"},
  {\"Effect\":\"Allow\",\"Action\":\"ses:SendEmail\",\"Resource\":\"*\",
   \"Condition\":{\"StringEquals\":{\"ses:FromAddress\":\"stan-claw@avriz.io\"}}}]}"
echo "role: may use the table and send as stan-claw@avriz.io"

ENV=$(MCP_TOKEN="$MCP_TOKEN" MCP_URL="$MCP_URL" FAMILY_EMAIL="$FAMILY_EMAIL" python3 -c '
import json, os
print(json.dumps({"Variables": {"MCP_TOKEN": os.environ["MCP_TOKEN"], "STATE_TABLE": "stan-mcp-demo-state",
  "MAIL_FROM": "stan-claw <stan-claw@avriz.io>", "PUBLIC_URL": os.environ["MCP_URL"].rstrip("/"),
  "FAMILY_EMAIL": os.environ["FAMILY_EMAIL"], "FAMILY_NAME": os.environ.get("FAMILY_NAME", "Reza")}}))')
aws lambda update-function-configuration --function-name $FN --region $R --timeout 20 --memory-size 256 \
  --environment "$ENV" >/dev/null
aws lambda wait function-updated --function-name $FN --region $R
echo "function: environment set, 20 s, 256 MB"

(cd stan-mcp && rm -f /tmp/stanmcp.zip && zip -q /tmp/stanmcp.zip lambda_function.py hoa_data.py family.py state.py mail.py)
aws lambda update-function-code --function-name $FN --region $R --zip-file fileb:///tmp/stanmcp.zip >/dev/null
aws lambda wait function-updated --function-name $FN --region $R
echo "function: code deployed"

RULE_ARN=$(aws events put-rule --name stan-mcp-weekly-summary --region $R \
  --schedule-expression "cron(0 14 ? * SUN *)" --query RuleArn --output text)
aws lambda add-permission --function-name $FN --region $R --statement-id weekly-summary \
  --action lambda:InvokeFunction --principal events.amazonaws.com --source-arn "$RULE_ARN" >/dev/null 2>&1 || true
aws events put-targets --rule stan-mcp-weekly-summary --region $R --targets \
  "Id=1,Arn=arn:aws:lambda:$R:$ACCT:function:$FN,Input='{\"weekly_summary\":true}'" >/dev/null
echo "schedule: Sundays 14:00 UTC (10 am EDT, 9 am EST)"
