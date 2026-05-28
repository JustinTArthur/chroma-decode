# AWS spot ephemeral GPU runners

`gpu-cuda.yml` and `gpu-amd.yml` launch single-use EC2 spot instances
from a GH-hosted bootstrap job, run the chromadec GPU validation on
them, then let the instance self-terminate.

| Runner | Region | AZ | Instance | GPU | AMI |
|---|---|---|---|---|---|
| NVIDIA | `ap-northeast-2` | `2d` | `g5.xlarge` | A10G 24GB | AWS DL Base GPU AMI (Ubuntu 22.04) |
| AMD | `us-east-2` | `2c` | `g4ad.xlarge` | Radeon Pro V520 (gfx1011) | stock Ubuntu 22.04 + `amdgpu-install` + `rocm/migraphx` Docker |

## One-time AWS setup

### 1. OIDC provider for GitHub Actions

```bash
aws iam create-open-id-connect-provider \
  --url https://token.actions.githubusercontent.com \
  --client-id-list sts.amazonaws.com \
  --thumbprint-list 6938fd4d98bab03faadb97b34396831e3780aea1
```

(AWS now validates the JWT chain natively, but the thumbprint field is
still required.)

### 2. IAM role that GH Actions assumes

Trust policy (`chd-gpu-runner-trust.json`):

```json
{
  "Version": "2012-10-17",
  "Statement": [{
    "Effect": "Allow",
    "Principal": {
      "Federated": "arn:aws:iam::ACCOUNT_ID:oidc-provider/token.actions.githubusercontent.com"
    },
    "Action": "sts:AssumeRoleWithWebIdentity",
    "Condition": {
      "StringEquals": {
        "token.actions.githubusercontent.com:aud": "sts.amazonaws.com"
      },
      "StringLike": {
        "token.actions.githubusercontent.com:sub": "repo:OWNER/REPO:*"
      }
    }
  }]
}
```

Replace `ACCOUNT_ID` with your AWS account ID and `OWNER/REPO` with the
GitHub `<owner>/<repo>` slug.

Permissions policy (`chd-gpu-runner-perms.json`):

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": [
        "ec2:RunInstances",
        "ec2:DescribeInstances",
        "ec2:DescribeImages",
        "ec2:DescribeSpotPriceHistory",
        "ec2:CreateTags",
        "ec2:TerminateInstances"
      ],
      "Resource": "*",
      "Condition": {
        "StringEqualsIfExists": {
          "ec2:Region": ["ap-northeast-2", "us-east-2"]
        }
      }
    }
  ]
}
```

Create the role:

```bash
aws iam create-role \
  --role-name ChdGpuRunnerLaunch \
  --assume-role-policy-document file://chd-gpu-runner-trust.json

aws iam put-role-policy \
  --role-name ChdGpuRunnerLaunch \
  --policy-name ChdGpuRunnerPerms \
  --policy-document file://chd-gpu-runner-perms.json
```

### 3. Set the repo variable

```bash
gh variable set AWS_GPU_RUNNER_ROLE_ARN \
  --body "arn:aws:iam::ACCOUNT_ID:role/ChdGpuRunnerLaunch"
```

(Or via the GitHub UI: Settings → Secrets and variables → Actions →
Variables.)

## Spot quotas

| Region | Quota | vCPUs needed | Status |
|---|---|---|---|
| `ap-northeast-2` | All G and VT Spot Instance Requests (`L-3819A6DF`) | 4 (min) / 8 (concurrency) | Request via Service Quotas |
| `us-east-2` | All G and VT Spot Instance Requests (`L-3819A6DF`) | 4 (min) / 8 (concurrency) | Default account often has 4 already |

## Cost (illustrative)

At current spot prices (May 2026): `g5.xlarge` in `ap-northeast-2` ≈
$0.37/hr, `g4ad.xlarge` in `us-east-2` ≈ $0.055/hr. A weekly run for
each, 10 min cold boot + 5 min build/test = $0.09/week NVIDIA + $0.014/week
AMD. Effectively rounding error.

## How the workflows self-clean

Each spot instance is launched with
`--instance-initiated-shutdown-behavior terminate`. The user-data
script ends with a `shutdown -h now` after the GH runner agent
finishes its one ephemeral job. A background `(sleep 1800 && shutdown
-h now)` guarantees the instance terminates after 30 minutes even if
the agent hangs. A `cleanup` job in each workflow runs `aws ec2
terminate-instances` on the tagged instance regardless of upstream
job outcome — belt-and-suspenders.