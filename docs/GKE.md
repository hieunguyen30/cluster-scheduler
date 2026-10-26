# Running Kestrel on GKE

**Status: written and statically checked only. Nothing here has been applied to Google Cloud.**
No `gcloud`, `terraform plan/apply`, Cloud Build run or image push has been executed. What HAS been
verified locally: the container image builds; the same manifests run on a local `kind` cluster
(chaos-tested); `terraform fmt` + `terraform validate` pass (offline); `kubectl kustomize` renders
the GKE overlay; the CI Docker stages (`scripts/ci_local.sh`) run the sanitizer test suites.

## What Terraform would create (`terraform/`)

`terraform plan` needs credentials and talks to Google Cloud, so it has NOT been run. By reading the
configuration, a plan with only `project_id` set should show **7 resources to add**:

| # | Resource | Notes |
|---|---|---|
| 1-4 | `google_project_service` x4 | container, artifactregistry, cloudbuild, monitoring (`disable_on_destroy=false`) |
| 5 | `google_artifact_registry_repository.images` | Docker repo, keeps 5 newest versions, deletes > 7 days |
| 6 | `google_container_cluster.kestrel` | **Autopilot**, regional, REGULAR channel, managed Prometheus on, `deletion_protection=false` |
| 7 | `google_monitoring_dashboard.kestrel` | 8 PromQL tiles over the `kestrel_*` metrics |

Optional (only if `github_owner` + `github_repo` / `build_service_account` are set):
`google_cloudbuild_trigger.ci` (CI on pushes to `main`) and an Artifact Registry writer IAM binding.
**Please run `terraform plan` yourself and compare it with this table before any `apply`.**

```bash
cd terraform
cp terraform.tfvars.example terraform.tfvars   # edit project_id
terraform init
terraform plan -var-file=terraform.tfvars      # <- review this
```

## Session runbook (keep it short)

```bash
# 1. once: infrastructure                        (terraform apply: needs your OK)
# 2. build + push the image (Cloud Build)        (costs build minutes)
gcloud builds submit --config cloudbuild.yaml --substitutions=SHORT_SHA=manual1 .
# 3. deploy
scripts/gke_deploy.sh PROJECT us-central1 manual1
scripts/gke_scale.sh up 3          # 3 workers per size = 9 workers
# ... run experiments (deploy/k8s/jobs/loadgen.yaml, kubectl delete pod ...) ...
scripts/gke_scale.sh down          # BACK TO ZERO PODS when you stop for any length of time
# 4. when the session is over:
terraform destroy -var-file=terraform.tfvars
```

Exact teardown command (also printed by `terraform output destroy_everything`):

```bash
cd terraform && terraform destroy -var project_id=PROJECT -var region=us-central1
```
It deletes the cluster (and with it the PVC holding the WAL), the Artifact Registry repository
(images are deleted too) and the dashboard. APIs stay enabled (`disable_on_destroy=false`).
Afterwards check the console for leftover persistent disks and load balancers (there should be none).

## Sizing and cost

* Control plane: 1 pod, requests 100m CPU / 128Mi, 10Gi `standard-rwo` PVC.
* Workers: pods request 50m CPU / 64Mi but advertise large LOGICAL capacities (2/4/8 cores). The jobs
  are `sleep`, so the pods do no real work. Autopilot's per-pod minimum is 50m / 52Mi **with
  bursting** and 250m / 512Mi without (Google's docs, fetched during development), so budget for
  the larger numbers.
* Idle: `scripts/gke_scale.sh down` takes every pod (and its billing) to zero; the PVC stays.

Worst-case request total with 9 workers at the 250m/512Mi floor: 10 pods x 250m = 2.5 vCPU and
10 x 512Mi = 5 GiB.

**Cost estimate: UNVERIFIED.** I could not retrieve the current price list (page not machine
readable), so the figures below come from memory of earlier public prices (~$0.0445 per vCPU-hour,
~$0.0049 per GiB-hour in us-central1, ~$0.10 per cluster-hour management fee with a free-tier
credit of about one cluster, PD ~$0.10/GiB-month). Plug the real numbers into the
[pricing calculator](https://cloud.google.com/products/calculator) before you approve anything.
With those remembered numbers a pod-resources bill is about 2.5 x 0.0445 + 5 x 0.0049 = ~$0.14 per
hour while the 10 pods exist, i.e. well under a dollar for a 2-hour session, plus Cloud Build
minutes for the sanitizer builds (the largest uncertain item: the ASan and TSan stages each compile
the whole project and gRPC-heavy tests).

## Why Autopilot

No node pools to size, nothing billed for an empty cluster except the management fee, and managed
Prometheus collection is on by default. The trade-off: you cannot control the node, and the
per-pod minimums make "tiny worker pods" cost more than their requests suggest.
