pub fn halt() -> ! {
    loop {
        core::hint::spin_loop();
    }
}
