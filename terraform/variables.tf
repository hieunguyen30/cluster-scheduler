variable "project_id" {
  description = "GCP project that will hold everything. Use a dedicated throwaway project."
  type        = string
}

variable "region" {
  description = "Autopilot clusters are regional. Pick the cheapest region near you (us-central1 is a common choice)."
  type        = string
  default     = "us-central1"
}

variable "cluster_name" {
  type    = string
  default = "kestrel"
}

variable "repository_id" {
  description = "Artifact Registry repository for the Kestrel image."
  type        = string
  default     = "kestrel"
}

variable "github_owner" {
  description = "GitHub owner for the Cloud Build trigger. Leave empty to skip creating the trigger (the Cloud Build GitHub app must already be connected to the project)."
  type        = string
  default     = ""
}

variable "github_repo" {
  description = "GitHub repository name for the Cloud Build trigger."
  type        = string
  default     = ""
}

variable "build_service_account" {
  description = "Email of the service account Cloud Build runs as (needs to push images). Leave empty to skip the IAM binding and grant it yourself."
  type        = string
  default     = ""
}
