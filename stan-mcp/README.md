# stan-mcp

An imaginary homeowners' association service, Sheppard Center HOA, as an MCP
server, for stan-claw to talk to. Everything in it is fiction. Tools:
`sign_in` (community + email; returns a homeowner token), `lookup_rule`,
`my_account`, `violations`, `file_request`, `next_board_meeting`,
`contact_management`. Account tools need the homeowner token; rules and
meetings are public.

Runs as the AWS Lambda `stan-mcp-demo` (us-east-1, Python 3.12) behind a
public function URL. Every request needs `Authorization: Bearer <MCP_TOKEN>`.
The URL and token live only in the gitignored `secrets/stan-mcp.env`.

    python3 stan-mcp/test_local.py                         # test the handler locally

    # update the code
    (cd stan-mcp && zip -q /tmp/stanmcp.zip lambda_function.py)
    aws lambda update-function-code --function-name stan-mcp-demo --region us-east-1 --zip-file fileb:///tmp/stanmcp.zip

    # point stan-claw at it (over USB)
    tools/stan-claw.py mcp url  "$(grep MCP_URL= secrets/stan-mcp.env | cut -d= -f2)"
    tools/stan-claw.py mcp token "$(grep MCP_TOKEN= secrets/stan-mcp.env | cut -d= -f2)"

    # take it all down
    aws lambda delete-function --function-name stan-mcp-demo --region us-east-1
    aws iam detach-role-policy --role-name stan-mcp-demo-lambda --policy-arn arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole
    aws iam delete-role --role-name stan-mcp-demo-lambda
