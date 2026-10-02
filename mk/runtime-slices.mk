# Shared runtime source slices, declared once. RX_RT_CORE_SRCS is the world,
# authority and evidence base that many rx_* test binaries link; RX_RT_OMEGA_SRCS
# is the omega core pair that follows it in the graph/query/plan/typed lists.
# Lists derive from these so the slice is not repeated; order is unchanged.
RX_RT_CORE_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c src/runtime/rx_native_bind.c src/runtime/rx_aegis.c src/sha256.c src/omega_evidence.c
RX_RT_OMEGA_SRCS = src/omega_core.c src/omega_canonical.c
