bool readPhysicsOriginSafely(void* physics, float origin[3]) {
    if (!physics || !origin) return false;
#if defined(_MSC_VER)
    __try {
#endif
        if (!readableMemory(physics, sizeof(void*))) return false;
        const auto vtable = *static_cast<void***>(physics);
        constexpr size_t getOriginSlot = 0x80 / sizeof(void*);
        if (!readableMemory(vtable, (getOriginSlot + 1) * sizeof(void*))) return false;
        const auto getOrigin = reinterpret_cast<PhysicsGetOriginFn>(vtable[getOriginSlot]);
        if (!executableMemory(reinterpret_cast<const void*>(getOrigin))) return false;
        const auto current = getOrigin(physics, 0);
        if (!readableMemory(current, 3 * sizeof(float))) return false;
        const float captured[3]{current[0], current[1], current[2]};
        if (!std::isfinite(captured[0]) || !std::isfinite(captured[1])
            || !std::isfinite(captured[2])) return false;
        for (int axis = 0; axis < 3; ++axis) origin[axis] = captured[axis];
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}
