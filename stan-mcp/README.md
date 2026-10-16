# stan-mcp

An imaginary homeowners' association service, Sheppard Center HOA, as an MCP
server, for stan-claw to talk to. Everything in it is fiction. Tools:
`sign_in` (community + email; returns a homeowner token), `lookup_rule`,
`my_account`, `violations`, `file_request`, `next_board_meeting`,
`contact_management`. Account tools need the homeowner token; rules and
meetings are public.

Family features (family.py): a homeowner's family contacts get messages
from them (`notify_family`), a Sunday summary if the homeowner agrees, and an
Approve / Decline link for any payment over $100; every email carries a
signed link to a page where family write a message that shows on the
stan-claw screen. State lives in the DynamoDB table `stan-mcp-demo-state`
(state.py); mail goes through SES from stan-claw@avriz.io (mail.py). The
board itself calls the hidden tools `reminders` and `family_inbox`.

Runs as the AWS Lambda `stan-mcp-demo` (us-east-1, Python 3.12) behind a
public function URL. Every request needs `Authorization: Bearer <MCP_TOKEN>`.
The URL and token live only in the gitignored `secrets/stan-mcp.env`.

    python3 stan-mcp/test_local.py                         # test the handler locally

    # family features: one-time AWS setup (state table, role policy, environment,
    # Sunday summary schedule), then the code -- see setup-care.sh
    sh stan-mcp/setup-care.sh

    # update the code
    (cd stan-mcp && zip -qr /tmp/stanmcp.zip lambda_function.py hoa_data.py family.py state.py mail.py library.py office.py civic.py library)
    aws lambda update-function-code --function-name stan-mcp-demo --region us-east-1 --zip-file fileb:///tmp/stanmcp.zip

    # point stan-claw at it (over USB)
    tools/stan-claw.py mcp url  "$(grep MCP_URL= secrets/stan-mcp.env | cut -d= -f2)"
    tools/stan-claw.py mcp token "$(grep MCP_TOKEN= secrets/stan-mcp.env | cut -d= -f2)"

    # take it all down
    aws events remove-targets --rule stan-mcp-weekly-summary --ids 1 --region us-east-1
    aws events delete-rule --name stan-mcp-weekly-summary --region us-east-1
    aws dynamodb delete-table --table-name stan-mcp-demo-state --region us-east-1
    aws iam delete-role-policy --role-name stan-mcp-demo-lambda --policy-name stan-mcp-demo-state-and-mail
    aws lambda delete-function --function-name stan-mcp-demo --region us-east-1
    aws iam detach-role-policy --role-name stan-mcp-demo-lambda --policy-arn arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole
    aws iam delete-role --role-name stan-mcp-demo-lambda
