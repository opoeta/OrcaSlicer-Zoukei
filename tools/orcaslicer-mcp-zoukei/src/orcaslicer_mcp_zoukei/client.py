"""HTTP client for the Orca Zoukei Remote API endpoints that the stock orcaslicer-mcp package lacks.

It subclasses the stock client so the connection settings (ORCA_API_URL, ORCA_API_TOKEN,
ORCA_API_TIMEOUT), the error mapping and the 127.0.0.1 token auth are exactly the ones the stock
tools use.
"""
from __future__ import annotations

from orcaslicer_mcp.client import OrcaClient
from orcaslicer_mcp.config import load_config
from orcaslicer_mcp.errors import ApiError, ConfigError


class ZoukeiClient(OrcaClient):
    async def call(self, method: str, path: str, body: dict | None = None, *, timeout: float = 120.0,
                   params: dict | None = None) -> dict:
        """One request to /api/v1/<path>; raises the stock ApiError subclasses on HTTP errors."""
        return await self._request(method, "/api/v1/" + path.lstrip("/"), json=body, params=params,
                                   timeout=timeout)


def zoukei_client() -> ZoukeiClient:
    try:
        return ZoukeiClient(load_config())
    except (RuntimeError, ValueError) as e:
        raise ConfigError(str(e))


__all__ = ["ZoukeiClient", "zoukei_client", "ApiError"]
