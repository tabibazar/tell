"""What stan-mcp remembers between calls: family helpers, consent for the weekly
summary, the homeowner's activity, payments waiting for a family OK, and
messages from family waiting for the screen.

One DynamoDB table (STATE_TABLE, keys pk and sk, both strings) holds it all,
each item's data a JSON string in `d`. With no STATE_TABLE (the local tests)
an in-memory dict stands in, so nothing here needs AWS to be tested.

  pk "hw#<community>|<email>"   sk "helper#<email>"     a family helper
                                sk "consent"            {"summary": bool}
                                sk "act#<time>#<rnd>"   one tool call (kept 90 days)
                                sk "msg#<time>#<id>"    a message for the screen
                                sk "appr#<id>"          a pointer to an approval
                                sk "photo#<time>#<id>"  a photo from family (the JPEG in `b`, binary)
  pk "approval#<id>"            sk "approval"           a payment waiting for a family OK
"""
import json
import os
import time

TABLE = os.environ.get("STATE_TABLE", "")
_mem = {}
_table = None


def _t():
    global _table
    if _table is None:
        import boto3
        _table = boto3.resource("dynamodb").Table(TABLE)
    return _table


def put(pk, sk, d, keep_days=None, blob=None):
    """blob: bytes kept beside the data (a photo), read back with get_blob."""
    item = {"pk": pk, "sk": sk, "d": json.dumps(d, ensure_ascii=False)}
    if blob is not None:
        item["b"] = blob
    if keep_days:
        item["ttl"] = int(time.time()) + keep_days * 86400
    if TABLE:
        _t().put_item(Item=item)
    else:
        _mem[(pk, sk)] = item


def get(pk, sk):
    if TABLE:
        item = _t().get_item(Key={"pk": pk, "sk": sk}).get("Item")
    else:
        item = _mem.get((pk, sk))
    return json.loads(item["d"]) if item else None


def get_blob(pk, sk):
    if TABLE:
        item = _t().get_item(Key={"pk": pk, "sk": sk}).get("Item")
        b = item.get("b") if item else None
        return bytes(b.value if hasattr(b, "value") else b) if b is not None else None
    item = _mem.get((pk, sk))
    return item.get("b") if item else None


def query(pk, prefix):
    """[(sk, data)] for pk's items whose sk starts with prefix, oldest first."""
    if TABLE:
        from boto3.dynamodb.conditions import Key
        out, kw = [], {"KeyConditionExpression": Key("pk").eq(pk) & Key("sk").begins_with(prefix),
                       "ProjectionExpression": "#p, #s, #d",          # never the photos themselves
                       "ExpressionAttributeNames": {"#p": "pk", "#s": "sk", "#d": "d"}}
        while True:
            r = _t().query(**kw)
            out += r["Items"]
            if "LastEvaluatedKey" not in r:
                break
            kw["ExclusiveStartKey"] = r["LastEvaluatedKey"]
    else:
        out = [v for (p, s), v in _mem.items() if p == pk and s.startswith(prefix)]
    return [(i["sk"], json.loads(i["d"])) for i in sorted(out, key=lambda i: i["sk"])]


def query_range(pk, lo, hi):
    """[(sk, data)] for pk's items with lo <= sk <= hi, in order."""
    if TABLE:
        from boto3.dynamodb.conditions import Key
        out, kw = [], {"KeyConditionExpression": Key("pk").eq(pk) & Key("sk").between(lo, hi),
                       "ProjectionExpression": "#p, #s, #d",
                       "ExpressionAttributeNames": {"#p": "pk", "#s": "sk", "#d": "d"}}
        while True:
            r = _t().query(**kw)
            out += r["Items"]
            if "LastEvaluatedKey" not in r:
                break
            kw["ExclusiveStartKey"] = r["LastEvaluatedKey"]
    else:
        out = [v for (p, s), v in _mem.items() if p == pk and lo <= s <= hi]
    return [(i["sk"], json.loads(i["d"])) for i in sorted(out, key=lambda i: i["sk"])]


def delete(pk, sk):
    if TABLE:
        _t().delete_item(Key={"pk": pk, "sk": sk})
    else:
        _mem.pop((pk, sk), None)


def reset():
    """Forget everything (local tests only)."""
    _mem.clear()
