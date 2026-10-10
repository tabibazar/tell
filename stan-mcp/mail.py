"""Email to family, through Amazon SES, from MAIL_FROM (an address on a domain
SES has verified). With no MAIL_FROM (the local tests) mail goes to `outbox`
instead, for the tests to read."""
import html
import os

MAIL_FROM = os.environ.get("MAIL_FROM", "")
outbox = []


def send(to, subject, text, links=()):
    """Plain text, and the same as simple HTML with `links` [(label, url)] as big
    buttons under it. Returns True if SES took it (or the outbox did)."""
    to = [t for t in to if t]
    if not to:
        return False
    if not MAIL_FROM:
        outbox.append({"to": to, "subject": subject, "text": text, "links": list(links)})
        return True
    body_text = text + "".join(f"\n\n{label}: {url}" for label, url in links)
    paras = "".join(f"<p style='margin:0 0 14px'>{html.escape(p)}</p>" for p in text.split("\n\n"))
    paras = paras.replace("\n", "<br>")
    buttons = "".join(
        f"<p style='margin:18px 0'><a href='{html.escape(url)}' style='background:#2f6fd0;color:#fff;"
        f"padding:12px 20px;border-radius:8px;text-decoration:none;font-weight:600'>{html.escape(label)}</a></p>"
        for label, url in links)
    body_html = (f"<div style='font-family:-apple-system,Segoe UI,Arial,sans-serif;font-size:17px;line-height:1.5;"
                 f"color:#1d2330;max-width:560px'>{paras}{buttons}"
                 f"<p style='color:#8a929e;font-size:13px;margin-top:28px'>Sent by stan-claw, the voice assistant, "
                 f"on behalf of the homeowner. A demo service: payments and requests are simulated.</p></div>")
    import boto3
    try:
        boto3.client("sesv2").send_email(
            FromEmailAddress=MAIL_FROM, Destination={"ToAddresses": to},
            Content={"Simple": {"Subject": {"Data": subject},
                                "Body": {"Text": {"Data": body_text}, "Html": {"Data": body_html}}}})
        return True
    except Exception as e:                      # noqa: BLE001 -- a failed email must not fail the tool call
        print("ses send failed:", e)
        return False
