"""nekodrop: an Apple ID identity for winawdl's AirDrop engine, for Contacts Only.

The client points NEKODROP_IDENTITY at a folder that holds what a Mac uses for
AirDrop: either WirDrop's appleid.exe wrote it after an Apple ID sign-in on this
PC, or the client converted what airdrop-keychain-extractor took from a Mac:

    certificate.der        the Apple ID AirDrop certificate (Apple-signed)
    key.pk8.der            its private key (PKCS#8 DER)
    validation_record.cms  the record that ties the certificate to the Apple ID
    intermediate.der       optional: the certificate that issued it
    contact_hashes.txt     optional: SHA-256 of the owner's contacts' emails and
                           phone numbers, one hex hash a line

With one, the engine presents that certificate on TLS (receiver and sender),
sends the record as ReceiverRecordData / SenderRecordData, and puts the
owner's hash prefixes into the Bluetooth beacon, the way a Mac does. iPhones
set to Contacts Only then list this PC when they know its owner.

NEKODROP_RECEIVE=contacts makes the receiver answer only senders whose record
names one of the owner's contacts or the owner's own Apple ID. The record is
not checked against Apple's signature here (no crypto library in the bundled
Python), so this is a filter, not proof.

Without a folder (or with an incomplete one) everything stays as before:
anonymous, visible to Everyone.
"""
import base64
import binascii
import logging
import os
import re

DIR = os.environ.get('NEKODROP_IDENTITY') or ''
CONTACTS_ONLY = os.environ.get('NEKODROP_RECEIVE') == 'contacts'
_NEEDED = ('certificate.der', 'key.pk8.der', 'validation_record.cms')


def _path(name):
    return os.path.join(DIR, name)


def present():
    return bool(DIR) and all(os.path.isfile(_path(n)) for n in _NEEDED)


def _read(name):
    try:
        with open(_path(name), 'rb') as f:
            return f.read()
    except OSError:
        return None


def record():
    """The validation record, read fresh: appleid.exe renews it while the engine runs."""
    return _read('validation_record.cms') if present() else None


def _pem(der, label):
    body = base64.b64encode(der).decode('ascii')
    lines = [body[i:i + 64] for i in range(0, len(body), 64)]
    return f'-----BEGIN {label}-----\n' + '\n'.join(lines) + f'\n-----END {label}-----\n'


def _pem_files():
    """The certificate chain and the key as PEM files beside the DER ones (ssl wants files)."""
    tls = _path('tls')
    os.makedirs(tls, exist_ok=True)
    chain = _pem(_read('certificate.der'), 'CERTIFICATE')
    intermediate = _read('intermediate.der')
    if intermediate:
        chain += _pem(intermediate, 'CERTIFICATE')
    certificate, key = os.path.join(tls, 'chain.pem'), os.path.join(tls, 'key.pem')
    with open(certificate, 'w', encoding='ascii') as f:
        f.write(chain)
    with open(key, 'w', encoding='ascii') as f:
        f.write(_pem(_read('key.pk8.der'), 'PRIVATE KEY'))
    return certificate, key


def load_into(context):
    """Presents the Apple ID certificate on this TLS context. False without an identity
    (or if it will not load), and the context is left as it was."""
    if not present():
        return False
    try:
        context.load_cert_chain(*_pem_files())
    except Exception as error:  # a bad key or certificate: stay anonymous rather than fail
        logging.warning('Apple ID identity unusable, staying anonymous: %s', error)
        return False
    return True


_HEX = re.compile(rb'<string>\s*([0-9A-Fa-f]{64})\s*</string>')
_DATA = re.compile(rb'<data>\s*([A-Za-z0-9+/=\s]+?)\s*</data>')


def _array(blob, key):
    """The body of the <array> after <key>key</key> in a record's XML plist."""
    at = blob.find(b'<key>' + key + b'</key>')
    if at < 0:
        return b''
    start = blob.find(b'<array>', at)
    end = blob.find(b'</array>', start)
    return blob[start:end] if start >= 0 and end > start else b''


def _hashes(body):
    out = [m.group(1).decode('ascii').lower() for m in _HEX.finditer(body)]
    for m in _DATA.finditer(body):
        try:
            raw = base64.b64decode(b''.join(m.group(1).split()))
        except (binascii.Error, ValueError):
            continue
        if len(raw) == 32:
            out.append(raw.hex())
    return out


def record_hashes(blob):
    """(email hashes, phone hashes) a record vouches for, as hex. Read without checking it."""
    if not blob:
        return [], []
    return _hashes(_array(blob, b'ValidatedEmailHashes')), _hashes(_array(blob, b'ValidatedPhoneHashes'))


def alt_dsid(blob):
    if not blob:
        return None
    m = re.search(rb'<key>altDsID</key>\s*<string>\s*([^<]+?)\s*</string>', blob)
    return m.group(1).decode('utf-8', 'replace') if m else None


def contacts():
    text = _read('contact_hashes.txt')
    if not text:
        return set()
    return {line.strip().lower() for line in text.decode('ascii', 'ignore').splitlines()
            if len(line.strip()) == 64}


def is_contact(sender_record):
    """Whether sender_record belongs to the owner's own Apple ID or contacts."""
    if not present() or not sender_record:
        return False
    own = alt_dsid(record())
    if own and alt_dsid(sender_record) == own:
        return True
    emails, phones = record_hashes(sender_record)
    known = contacts()
    return any(h in known for h in emails + phones)


def allowed(sender_record):
    """Whether the receiver answers this sender: always for Everyone; for Contacts Only,
    the owner's own devices and the owner's contacts."""
    if not CONTACTS_ONLY or not present():
        return True
    return is_contact(sender_record)


def receiver_extra():
    blob = record()
    return {'ReceiverRecordData': blob} if blob else {}


def sender_extra():
    blob = record()
    return {'SenderRecordData': blob} if blob else {}


def ble_hashes():
    """The beacon's four 2-byte hash prefixes (Apple ID, phone, email, second email), as hex."""
    emails, phones = record_hashes(record())

    def first2(values, i):
        return values[i][:4] if len(values) > i else '0000'
    return first2(emails, 0) + first2(phones, 0) + first2(emails, 0) + first2(emails, 1)


def sender_body(value):
    """The plist a sender posts, with its record when it has one."""
    return {**value, **sender_extra()}


if present():
    # The Bluetooth beacon script is a child of this process: it reads the prefixes here.
    os.environ['NEKODROP_BLE_HASHES'] = ble_hashes()
    emails, phones = record_hashes(record())
    logging.getLogger(__name__).info(
        'Apple ID identity: %d email and %d phone hashes, %d contacts, receiving from %s',
        len(emails), len(phones), len(contacts()), 'contacts' if CONTACTS_ONLY else 'everyone')

__all__ = ['present', 'record', 'load_into', 'allowed', 'receiver_extra', 'sender_extra', 'sender_body',
           'record_hashes', 'alt_dsid', 'contacts', 'ble_hashes']
