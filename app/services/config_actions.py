"""All GUI configuration mutations enter the authoritative control process."""

from __future__ import annotations

from . import config_store


async def change(action: str, *args, **kwargs):
    functions = {"save": config_store.save, "set_enable": config_store.set_enable,
                 "set_main_field": config_store.set_main_field,
                 "set_main_fields": config_store.set_main_fields,
                 "apply_device_ip": config_store.apply_device_ip,
                 "apply_gox_acquisition": config_store.apply_gox_acquisition,
                 "apply_fx10_acquisition": config_store.apply_fx10_acquisition}
    return functions[action](*args, **kwargs)
