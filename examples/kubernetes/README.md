# Kubernetes Example: SQLite to PostgreSQL Hook

This example demonstrates how to use `postgres-sqlite` in a Kubernetes environment to redirect an application`s SQLite calls to a PostgreSQL database without modifying the application image.

## How it works

1.  **Init Container**: Downloads the `sqlite_hook.so` library and installs `libpq5` (the PostgreSQL client library). It then copies the hook and `libpq.so.5` to a shared `emptyDir` volume.
2.  **Environment Variables**:
    *   `LD_PRELOAD`: Tells the dynamic linker to load our hook before any other libraries.
    *   `LD_LIBRARY_PATH`: Ensures the linker can find `libpq.so.5` inside the shared volume.
    *   `PG_CONNINFO`: Provides the PostgreSQL connection string used by the hook.
3.  **Volume Sharing**: Both the init container and the main container mount the `patch-libs` volume to share the library files.

## Files

- `deployment.yaml`: A sample Kubernetes Deployment manifest.

## Usage

1.  Customize the `PG_CONNINFO` in `deployment.yaml`.
2.  Ensure your main container image is based on an OS compatible with the downloaded `.so` (e.g., Ubuntu 24.04).
3.  Apply the manifest:
    ```bash
    kubectl apply -f deployment.yaml
    ```
