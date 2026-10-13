locals {
  services = [
    "container.googleapis.com",
    "artifactregistry.googleapis.com",
    "cloudbuild.googleapis.com",
    "monitoring.googleapis.com",
  ]
}

resource "google_project_service" "enabled" {
  for_each = toset(local.services)
  service  = each.value

  # Destroying this stack must not disable APIs other things may use.
  disable_on_destroy = false
}

resource "google_artifact_registry_repository" "images" {
  location      = var.region
  repository_id = var.repository_id
  format        = "DOCKER"
  description   = "Kestrel container images"

  # Keep storage cost bounded: only the 5 newest versions of each image are kept.
  cleanup_policies {
    id     = "keep-recent"
    action = "KEEP"
    most_recent_versions {
      keep_count = 5
    }
  }
  cleanup_policies {
    id     = "delete-old"
    action = "DELETE"
    condition {
      older_than = "604800s" # 7 days
    }
  }

  depends_on = [google_project_service.enabled]
}

# Autopilot: no node pools to size or pay for while idle; billing is per pod resource request.
# Managed Service for Prometheus collection is always on for Autopilot (>= 1.25).
resource "google_container_cluster" "kestrel" {
  name     = var.cluster_name
  location = var.region

  enable_autopilot    = true
  deletion_protection = false # so `terraform destroy` works; this is a throwaway demo cluster

  release_channel {
    channel = "REGULAR"
  }

  # Empty block = let GKE choose secondary ranges (required for Autopilot VPC-native clusters).
  ip_allocation_policy {}

  monitoring_config {
    managed_prometheus {
      enabled = true
    }
  }

  depends_on = [google_project_service.enabled]
}

# Cloud Build needs permission to push to Artifact Registry.
resource "google_artifact_registry_repository_iam_member" "build_push" {
  count      = var.build_service_account == "" ? 0 : 1
  location   = google_artifact_registry_repository.images.location
  repository = google_artifact_registry_repository.images.name
  role       = "roles/artifactregistry.writer"
  member     = "serviceAccount:${var.build_service_account}"
}

# CI: build, run unit tests under ASan and TSan, push the image. Only created when a GitHub repo
# is configured, because the trigger needs the Cloud Build GitHub app connection.
resource "google_cloudbuild_trigger" "ci" {
  count       = var.github_owner == "" || var.github_repo == "" ? 0 : 1
  name        = "kestrel-ci"
  description = "Build, test (ASan/TSan) and push Kestrel"
  location    = "global"
  filename    = "cloudbuild.yaml"

  github {
    owner = var.github_owner
    name  = var.github_repo
    push {
      branch = "^main$"
    }
  }

  substitutions = {
    _REGION = var.region
    _REPO   = var.repository_id
  }

  service_account = var.build_service_account == "" ? null : "projects/${var.project_id}/serviceAccounts/${var.build_service_account}"

  depends_on = [google_project_service.enabled]
}

# Cloud Monitoring dashboard over the PromQL series Managed Prometheus ingests from /metrics.
resource "google_monitoring_dashboard" "kestrel" {
  dashboard_json = file("${path.module}/dashboard.json")
  depends_on     = [google_project_service.enabled]
}
