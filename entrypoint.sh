#!/usr/bin/env bash
# Recorder container entrypoint.
#
# The KVS C SDK only reads STATIC AWS creds from env (AWS_ACCESS_KEY_ID /
# AWS_SECRET_ACCESS_KEY / AWS_SESSION_TOKEN) — it does not understand the ECS task
# role's container-credentials endpoint. So we fetch the task-role creds here and
# export them before exec'ing the recorder. (The AWS CLI used for the S3 upload picks
# up the task role on its own, so this is only needed for the SDK.)
set -euo pipefail

export AWS_DEFAULT_REGION="${AWS_REGION:-us-east-1}"
# The KVS SDK verifies AWS endpoints against this bundle (shipped with the SDK).
export AWS_KVS_CACERT_PATH="/opt/kvs/cert.pem"
export LD_LIBRARY_PATH="/opt/kvs/lib:${LD_LIBRARY_PATH:-}"

# Make sure the SDK takes the static-credential path, not the device IoT-cert path.
unset AWS_IOT_CORE_CREDENTIAL_ENDPOINT 2>/dev/null || true

if [ -n "${AWS_CONTAINER_CREDENTIALS_RELATIVE_URI:-}" ]; then
  creds="$(curl -sf "http://169.254.170.2${AWS_CONTAINER_CREDENTIALS_RELATIVE_URI}")"
  AWS_ACCESS_KEY_ID="$(printf '%s' "$creds" | jq -r .AccessKeyId)"
  AWS_SECRET_ACCESS_KEY="$(printf '%s' "$creds" | jq -r .SecretAccessKey)"
  AWS_SESSION_TOKEN="$(printf '%s' "$creds" | jq -r .Token)"
  export AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY AWS_SESSION_TOKEN
elif [ -n "${AWS_CONTAINER_CREDENTIALS_FULL_URI:-}" ]; then
  creds="$(curl -sf "${AWS_CONTAINER_CREDENTIALS_FULL_URI}")"
  AWS_ACCESS_KEY_ID="$(printf '%s' "$creds" | jq -r .AccessKeyId)"
  AWS_SECRET_ACCESS_KEY="$(printf '%s' "$creds" | jq -r .SecretAccessKey)"
  AWS_SESSION_TOKEN="$(printf '%s' "$creds" | jq -r .Token)"
  export AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY AWS_SESSION_TOKEN
fi

if [ -z "${KVS_CHANNEL_NAME:-}" ]; then
  echo "[entrypoint] KVS_CHANNEL_NAME is required" >&2
  exit 1
fi

# The bundled aws-cli v1 defaults `iot-data` to the deprecated non-ATS endpoint
# (data.iot.<region>.amazonaws.com), whose cert is no longer trusted, so the recorder's
# "capturing" publish would fail silently. Resolve the account's ATS data endpoint here
# (a control-plane call, which works) and export it; the recorder passes it to
# `aws iot-data publish --endpoint-url`. Non-fatal if it can't be resolved.
IOT_DATA_ENDPOINT="$(aws iot describe-endpoint --endpoint-type iot:Data-ATS --query endpointAddress --output text 2>/dev/null || true)"
export IOT_DATA_ENDPOINT
echo "[entrypoint] IoT data endpoint: ${IOT_DATA_ENDPOINT:-<unresolved>}"

echo "[entrypoint] starting recorder for channel ${KVS_CHANNEL_NAME} (region ${AWS_DEFAULT_REGION})"
# exec so SIGTERM from ECS StopTask reaches the recorder directly (graceful finalize).
exec /usr/local/bin/xorgate-kvs-recorder "${KVS_CHANNEL_NAME}" video-only
