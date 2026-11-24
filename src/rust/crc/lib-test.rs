use super::*;

#[test]
fn check_value() {
    assert_eq!(crc64nvme(0, b"123456789"), 0xae8b_1486_0a79_9888);
    assert_eq!(crc64nvme(0, b""), 0);
}

#[test]
fn split_input() {
    let data: Vec<u8> = (0..5000u32)
        .map(|i| (i.wrapping_mul(2_654_435_761) >> 24) as u8)
        .collect();
    let whole = crc64nvme(0, &data);
    for split in [0, 1, 15, 16, 17, 255, 256, 4999, 5000] {
        assert_eq!(
            crc64nvme(crc64nvme(0, &data[..split]), &data[split..]),
            whole
        );
    }
}
