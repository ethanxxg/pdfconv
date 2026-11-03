"""Regenerates tests/data/sample.eml (third-party fixture for P9 email / F-501).

A multipart/mixed message with:
  - a multipart/alternative body (text/plain + text/html)
  - an RFC 2047 encoded-word Subject
  - two attachments: a PNG named "photo.bin" (extension mismatches content, so the
    reader must report the real image/png via content sniffing) and a notes.txt.

Built by hand so the boundaries, transfer encodings and encoded-words are explicit.
Run:  python tests/make_sample_eml.py
"""
import base64
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "sample.eml")

# 1x1 PNG（内容签名 image/png，但附件名故意叫 photo.bin）。
PNG_1x1 = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR42mNk+M9QDwADhgGA"
    "WjR9awAAAABJRU5ErkJggg=="
)

# Subject: "Hello 世界" -> RFC 2047 encoded-word (base64, UTF-8).
_subject_b64 = base64.b64encode("Hello 世界".encode("utf-8")).decode("ascii")
SUBJECT_EW = "=?UTF-8?B?" + _subject_b64 + "?="


def main():
    png_b64 = base64.b64encode(PNG_1x1).decode("ascii")
    notes_b64 = base64.b64encode("second attachment body: 第二附件".encode("utf-8")).decode("ascii")

    text = (
        "From: \"Alice Example\" <alice@example.com>\r\n"
        "To: Bob <bob@example.com>\r\n"
        "Cc: carol@example.com\r\n"
        f"Subject: {SUBJECT_EW}\r\n"
        "Date: Wed, 21 Oct 2026 10:00:00 +0800\r\n"
        "Message-ID: <demo-0001@example.com>\r\n"
        "MIME-Version: 1.0\r\n"
        'Content-Type: multipart/mixed; boundary="OUTER"\r\n'
        "\r\n"
        "--OUTER\r\n"
        'Content-Type: multipart/alternative; boundary="INNER"\r\n'
        "\r\n"
        "--INNER\r\n"
        'Content-Type: text/plain; charset="UTF-8"\r\n'
        "\r\n"
        "This is the plain-text body.\r\n"
        "--INNER\r\n"
        'Content-Type: text/html; charset="UTF-8"\r\n'
        "\r\n"
        "<html><body><p>This is the <b>HTML</b> body.</p></body></html>\r\n"
        "--INNER--\r\n"
        "--OUTER\r\n"
        'Content-Type: image/png; name="photo.bin"\r\n'
        'Content-Disposition: attachment; filename="photo.bin"\r\n'
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        f"{png_b64}\r\n"
        "--OUTER\r\n"
        'Content-Type: text/plain; name="notes.txt"\r\n'
        'Content-Disposition: attachment; filename="notes.txt"\r\n'
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        f"{notes_b64}\r\n"
        "--OUTER--\r\n"
    )

    with open(OUT, "wb") as f:
        f.write(text.encode("utf-8"))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
