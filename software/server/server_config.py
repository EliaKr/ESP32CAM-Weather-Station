# ---- bind ----
HOST = "0.0.0.0"
PORT = 8080
INGEST_PATH = "/ingest"

# ---- auth tokens (token -> friendly name) ----
# Use long random strings. Treat them as shared secrets.
AUTH_TOKENS = {
    "CHANGE_ME_TOKEN_1": "pico2w-01",
    "CHANGE_ME_TOKEN_2": "pico2w-02",
}

# If true, server will accept either:
# - JSON field "auth_token"
# - OR HTTP header "Authorization: Bearer <token>"
ALLOW_BEARER_HEADER = True

# ---- csv ----
CSV_PATH_1 = "./processing/server_log_1.csv"
CSV_PATH_2 = "./path/archive/server_log_2.csv"

CSV_HEADER = [
    "server_ts_utc",
    "device_id",
    "ms",
    "temp_c",
    "hum_pct",
    "press_hpa",
    "gas_ohms",
    "alt_m",
    "vbat_v",
    "vsolar_v",
    "als_lux",
    "white",
    "sgp41_sraw_voc",
    "sgp41_sraw_nox",
    "sgp41_voc_index",
    "sgp41_nox_index",
    "sgp41_ok",
]

APPEND_IF_EXISTS = True

# ---- logging ----
VERBOSE = True

# ---- overload protections ----
MAX_BODY_BYTES = 4096
MAX_CONCURRENT_REQUESTS = 20

RATE_LIMIT_ENABLED = True
RATE_LIMIT_IP_RPS = 2.0
RATE_LIMIT_IP_BURST = 10
RATE_LIMIT_TOKEN_RPS = 1.0
RATE_LIMIT_TOKEN_BURST = 5

# ---- HMAC protection ----
# Require X-Signature: hex(hmac_sha256(token_secret, body_bytes))
# Optional: also require X-Timestamp and enforce a window.
REQUIRE_HMAC = True
REQUIRE_TIMESTAMP = False
TIMESTAMP_MAX_SKEW_SEC = 120  # accept ±2 minutes
