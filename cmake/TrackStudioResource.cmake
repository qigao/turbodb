# Compile the revision with Studio so Ninja relinks when the PE resource changes.
file(SHA256 "${RESOURCE}" digest)
file(CONFIGURE OUTPUT "${REVISION}" CONTENT
  "typedef char turbodb_studio_resource_revision_@digest@[1];
" @ONLY)
