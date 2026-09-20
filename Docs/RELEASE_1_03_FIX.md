# KHARVOX 1.03 Movement Fix Test

Experimental movement fix; root cause of the reported slowdown remains unconfirmed.

- Publish left-stick X/Y atomically so DOOM cannot read a mixed movement vector.
- Preserve analog input through 50% travel, smoothly raise the upper range, and reach full magnitude at 90% stick travel. Direction is preserved for cardinal and diagonal movement and Head/Offhand modes.
- Apply travel compensation only to manual gameplay movement; menu, weapon-wheel and synthesized room-scale input retain their existing handling.
- Remove the 1.03 movement CSV recording. No logs are required for feedback.

Try forward/backward/strafe/diagonal movement in the problem areas and report whether full speed is consistent and slow movement still feels controllable. This remains an XInput-based path; no direct native movement injection is used.
