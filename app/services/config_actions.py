"""All GUI configuration mutations enter the authoritative control process."""

from __future__ import annotations

from . import config_store, control_client


async def change(action: str, *args, **kwargs):
    if control_client.enabled():
        return await control_client.call("config." + action, *args, **kwargs)
    functions = {"save": config_store.save, "set_enable": config_store.set_enable,
                 "apply_device_ip": config_store.apply_device_ip,
                 "apply_gox_acquisition": config_store.apply_gox_acquisition,
                 "apply_fx10_acquisition": config_store.apply_fx10_acquisition}
    return functions[action](*args, **kwargs)
