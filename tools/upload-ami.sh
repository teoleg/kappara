#!/usr/bin/env bash
#
# tools/upload-ami.sh -- upload build/kappara-ami.img to S3 and
# register it as an EC2 AMI in the current AWS account / region.
#
# Wraps the four-step deploy dance:
#   1. aws s3 cp                                      (raw image upload)
#   2. aws ec2 import-snapshot                        (raw -> EBS snapshot)
#   3. aws ec2 describe-import-snapshot-tasks         (poll until ready)
#   4. aws ec2 register-image                         (snapshot -> AMI)
#
# Prerequisites (one-time, AWS-side):
#   - S3 bucket exists in the region we'll register the AMI in
#   - vmimport IAM service role exists with read access to the bucket;
#     see docs/AWS.md "Pushing to AWS" for the trust + role policies
#   - AWS credentials configured (aws configure, env vars, or an
#     instance role) with permission to s3:PutObject + ec2:Import*
#     + ec2:RegisterImage
#
# Usage:
#   tools/upload-ami.sh --bucket BUCKET --name AMI_NAME [--region REGION]
#                       [--image PATH]
#
# Writes the resulting AMI ID to build/last-ami-id.txt (consumed by
# the CI workflow to comment on the release).

set -euo pipefail

IMAGE=build/kappara-ami.img
BUCKET=""
NAME=""
REGION="${AWS_REGION:-}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bucket) BUCKET="$2"; shift 2;;
        --name)   NAME="$2";   shift 2;;
        --region) REGION="$2"; shift 2;;
        --image)  IMAGE="$2";  shift 2;;
        *) echo "upload-ami: unknown arg '$1'" >&2; exit 1;;
    esac
done

if [[ -z "$BUCKET" || -z "$NAME" ]]; then
    echo "usage: $0 --bucket BUCKET --name AMI_NAME [--region REGION] [--image PATH]" >&2
    exit 1
fi

if [[ ! -f "$IMAGE" ]]; then
    echo "upload-ami: image not found: $IMAGE  (run \`make ami\` first)" >&2
    exit 1
fi

for t in aws jq; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "upload-ami: missing tool '$t'" >&2
        exit 1
    }
done

REGION_ARG=()
if [[ -n "$REGION" ]]; then REGION_ARG=(--region "$REGION"); fi

KEY="kappara-ami/$(date -u +%Y%m%dT%H%M%S)-${NAME}.img"

echo "upload-ami: 1/4 uploading $IMAGE -> s3://$BUCKET/$KEY"
aws s3 cp "$IMAGE" "s3://$BUCKET/$KEY" "${REGION_ARG[@]}"

echo "upload-ami: 2/4 starting ec2 import-snapshot"
TASK_ID=$(aws ec2 import-snapshot \
    "${REGION_ARG[@]}" \
    --description "kappara $NAME" \
    --disk-container "{
        \"Format\":\"raw\",
        \"UserBucket\":{\"S3Bucket\":\"$BUCKET\",\"S3Key\":\"$KEY\"}
    }" \
    --query ImportTaskId --output text)
echo "upload-ami:     ImportTaskId=$TASK_ID"

echo "upload-ami: 3/4 polling import-snapshot status (this can take 5-10 min)"
START=$SECONDS
while :; do
    STATE=$(aws ec2 describe-import-snapshot-tasks \
        "${REGION_ARG[@]}" \
        --import-task-ids "$TASK_ID" \
        --query 'ImportSnapshotTasks[0].SnapshotTaskDetail.Status' \
        --output text)
    PROGRESS=$(aws ec2 describe-import-snapshot-tasks \
        "${REGION_ARG[@]}" \
        --import-task-ids "$TASK_ID" \
        --query 'ImportSnapshotTasks[0].SnapshotTaskDetail.Progress' \
        --output text 2>/dev/null || echo "?")
    ELAPSED=$((SECONDS - START))
    printf "upload-ami:     [%4ds] status=%-12s progress=%s\n" \
           "$ELAPSED" "$STATE" "$PROGRESS"
    case "$STATE" in
        completed) break;;
        deleting|deleted|cancelled|cancelling)
            echo "upload-ami: import failed (state=$STATE)" >&2
            exit 1;;
    esac
    if [[ $ELAPSED -gt 1800 ]]; then
        echo "upload-ami: import timed out after 30 minutes" >&2
        exit 1
    fi
    sleep 15
done

SNAPSHOT_ID=$(aws ec2 describe-import-snapshot-tasks \
    "${REGION_ARG[@]}" \
    --import-task-ids "$TASK_ID" \
    --query 'ImportSnapshotTasks[0].SnapshotTaskDetail.SnapshotId' \
    --output text)
echo "upload-ami:     SnapshotId=$SNAPSHOT_ID"

echo "upload-ami: 4/4 registering AMI"
AMI_ID=$(aws ec2 register-image \
    "${REGION_ARG[@]}" \
    --name "$NAME" \
    --description "kappara $NAME (SVR4-flavored aarch64 UEFI)" \
    --architecture arm64 \
    --boot-mode uefi \
    --root-device-name /dev/sda1 \
    --virtualization-type hvm \
    --ena-support \
    --block-device-mappings "DeviceName=/dev/sda1,Ebs={SnapshotId=$SNAPSHOT_ID,VolumeSize=1,DeleteOnTermination=true,VolumeType=gp3}" \
    --query ImageId --output text)

echo "upload-ami: done -- AmiId=$AMI_ID"
mkdir -p build
echo "$AMI_ID" > build/last-ami-id.txt

# Friendly hint for the next step.  Pinning instance-type to a Graviton
# family avoids the "tried to launch on c5 and got 'invalid arch'"
# trap people hit with the first arm64 AMI in their account.
cat <<EOF

Next: launch on Graviton (c7g, t4g, m7g, r7g families).  Smallest
working choice:

  aws ec2 run-instances ${REGION_ARG[@]+"${REGION_ARG[@]}"} \\
      --image-id $AMI_ID \\
      --instance-type t4g.small \\
      --key-name YOUR_KEY \\
      --enable-api-stop \\
      --tag-specifications 'ResourceType=instance,Tags=[{Key=Name,Value=$NAME}]'

The instance has no working network yet (Stage E ENA driver is
the blind impl).  Watch the boot via EC2 Serial Console:

  aws ec2 get-serial-console-access-status ${REGION_ARG[@]+"${REGION_ARG[@]}"}
  aws ec2-instance-connect send-serial-console-ssh-public-key ...
EOF
