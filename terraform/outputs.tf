output "cluster_name" {
  value = google_container_cluster.kestrel.name
}

output "image_repository" {
  description = "Push/pull path prefix for the Kestrel image."
  value       = "${var.region}-docker.pkg.dev/${var.project_id}/${var.repository_id}"
}

output "get_credentials" {
  value = "gcloud container clusters get-credentials ${google_container_cluster.kestrel.name} --region ${var.region} --project ${var.project_id}"
}

output "destroy_everything" {
  description = "Run this when the session is over. It deletes the cluster (and with it the PVC), the image repository and the dashboard."
  value       = "terraform destroy -var project_id=${var.project_id} -var region=${var.region}"
}
