# SPDX-License-Identifier: GPL-2.0-or-later
"""Local identity chain: simulated PSN -> OAuth code/token -> UFC Blaze session.

LoginResponse/UserLoginInfo/PersonaDetails tags come from UFC's TDF reflection,
not an unmodified BF4 or Blaze3 login packet. This is a development LAN backend.
"""
from dataclasses import dataclass
import secrets
import time

from blaze_tdf import INT, STRING, BLOB, STRUCT, UNION, OBJID, Struct, Union, encode_struct

CLIENT_ID = 'UFC2018_PS4_CLIENT'
SERVER_ID = 'UFC2018_PS4_SERVER'
PS4_PLATFORM = 20  # UFC ClientPlatformType reflection: pc=4, ps4=20


@dataclass
class Account:
    id: int
    psn_id: int
    name: str


class IdentityStore:
    def __init__(self):
        self.accounts = {}
        self.codes = {}
        self.tokens = {}

    def account(self, psn_id, name):
        if not 0 < psn_id < (1 << 63):
            raise ValueError('invalid simulated PSN account')
        if psn_id not in self.accounts:
            clean = ''.join(c for c in name[:16] if c.isascii() and (c.isalnum() or c in '_-'))
            self.accounts[psn_id] = Account(1000000000 + len(self.accounts) + 1, psn_id, clean or 'Player')
        return self.accounts[psn_id]

    def issue_code(self, account, client_id):
        if client_id not in (CLIENT_ID, SERVER_ID):
            raise ValueError('unsupported OAuth client')
        now = time.monotonic()
        self.codes = {k: v for k, v in self.codes.items() if v[2] > now}
        code = 'UFC3CODE' + secrets.token_hex(16)
        self.codes[code] = (account, client_id, now + 300)
        return code

    def redeem_code(self, code, client_id, psn_id):
        record = self.codes.get(code)
        if not record or record[1] != client_id or record[0].psn_id != psn_id or record[2] <= time.monotonic():
            raise ValueError('invalid or expired OAuth code')
        del self.codes[code]
        return record[0]

    def exchange_token(self, code, client_id, psn_id):
        account = self.redeem_code(code, client_id, psn_id)
        token = 'UFC3TOKEN' + secrets.token_hex(24)
        self.tokens[token] = (account, client_id, time.monotonic() + 86400)
        return token

    def token_account(self, token, psn_id):
        record = self.tokens.get(token)
        if not record or record[0].psn_id != psn_id or record[2] <= time.monotonic():
            raise ValueError('invalid or expired access token')
        return record[0], record[1]

    def login(self, request):
        def field(tag, kind):
            item = request.get(tag) if request else None
            if not item or item[0] != kind:
                raise ValueError('missing login field ' + tag)
            return item[1]
        account = self.redeem_code(field('AUTH', STRING), SERVER_ID, field('EXTI', INT))
        key = secrets.token_hex(32)
        now = int(time.time())
        persona = Struct(DSNM=(STRING, account.name), LAST=(INT, now), PID=(INT, account.id),
                         PLAT=(INT, PS4_PLATFORM), STAS=(INT, 1), XREF=(INT, account.psn_id))
        session = Struct({'1CON': (INT, 0), 'BUID': (INT, account.id), 'FRST': (INT, 0),
                          'GEO': (INT, 0), 'KEY': (STRING, key), 'LLOG': (INT, now),
                          'MAIL': (STRING, ''), 'PDTL': (STRUCT, persona), 'UID': (INT, account.id)})
        reply = encode_struct(Struct(ANON=(INT, 0), SESS=(STRUCT, session), SPAM=(INT, 0), UNDR=(INT, 0)))
        return account, key, reply


def user_authenticated(account, key):
    """UserSessions notification 8, UFC's UserSessionLoginInfo schema.

    BF4 demonstrates the reply + notification flow; UFC reflection uses 1CON
    instead of FRSC. CGID is the connection group (UserSessions 0x7802, entity type 2,
    the account id): GameManager tells the members of a game apart by it, and two
    clients with the default 0 both take themselves for the game's host.
    """
    now = int(time.time())
    return encode_struct([
        ('1CON', INT, 0), ('ALOC', INT, 1701729619), ('BUID', INT, account.id),
        ('CGID', OBJID, (0x7802, 2, account.id)), ('DSNM', STRING, account.name), ('FRST', INT, 0), ('KEY', STRING, key),
        ('LAST', INT, now), ('LLOG', INT, now), ('MAIL', STRING, ''),
        ('NASP', STRING, 'cem_ea_id'), ('PID', INT, account.id), ('PLAT', INT, PS4_PLATFORM),
        ('UID', INT, account.id), ('USTP', INT, 0), ('XREF', INT, account.psn_id),
    ])


def telemetry_config():
    return Struct(ADRS=(STRING, ''), ANON=(INT, 0), DISA=(STRING, 'ALL'),
                       FILT=(STRING, ''), LOC=(INT, 1701729619), NOOK=(STRING, ''),
                       PORT=(INT, 0), SDLY=(INT, 15000), SESS=(STRING, ''), SKEY=(STRING, ''),
                       SPCT=(INT, 0), STIM=(STRING, 'Default'))


def post_auth(account):
    """UFC PostAuthResponse contains TELE, TICK, UROP (no BF4 PSS).

    Ancillary telemetry/ticker services are disabled; no real EA endpoints are
    advertised. The local session is identified by the same Blaze UID.
    """
    ticker = Struct(ADRS=(STRING, ''), PORT=(INT, 0), SKEY=(STRING, ''))
    options = Struct(TMOP=(INT, 0), UID=(INT, account.id))
    return encode_struct(Struct(TELE=(STRUCT, telemetry_config()), TICK=(STRUCT, ticker), UROP=(STRUCT, options)))


def user_added(account, address=None):
    # UFC UserIdentification has nine fields. The EXID is the simulated PSN
    # account, while AID/ID/ORIG/PIDI refer to the local backend account/persona.
    user = Struct(AID=(INT, account.id), ALOC=(INT, 1701729619), EXBB=(BLOB, b''),
                  EXID=(INT, account.psn_id), ID=(INT, account.id), NAME=(STRING, account.name),
                  NASP=(STRING, 'cem_ea_id'), ORIG=(INT, account.id), PIDI=(INT, account.id))
    # The client supplies its network address later via updateNetworkInfo; do
    # not infer a reachable P2P address from the Blaze TCP connection's port.
    data = Struct(ADDR=(UNION, address or Union()), QDAT=(STRUCT, Struct(DBPS=(INT, 0), NATT=(INT, 0), UBPS=(INT, 0))))
    return encode_struct(Struct(DATA=(STRUCT, data), USER=(STRUCT, user)))


def user_updated(account):
    return encode_struct(Struct(FLGS=(INT, 3), ID=(INT, account.id)))  # SUBSCRIBED | ONLINE
