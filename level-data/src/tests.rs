use super::*;

fn near(a: f32, b: f32, tolerance: f32) -> bool {
    (a - b).abs() <= tolerance
}

fn attitude(ax: f32, ay: f32, az: f32) -> Attitude {
    attitude_from_accel(ax, ay, az).unwrap_or(Attitude {
        pitch: f32::NAN,
        roll: f32::NAN,
    })
}

#[test]
fn flat_reads_zero_either_face_up() {
    for az in [1.0, -1.0] {
        let a = attitude(0.0, 0.0, az);
        assert!(near(a.pitch, 0.0, 1e-4) && near(a.roll, 0.0, 1e-4), "{a:?}");
    }
}

#[test]
fn raised_top_edge_is_positive_pitch_and_raised_right_edge_positive_roll() {
    let (s, c) = 5.0_f32.to_radians().sin_cos();
    let top = attitude(-s, 0.0, c);
    assert!(
        near(top.pitch, 5.0, 0.01) && near(top.roll, 0.0, 0.01),
        "{top:?}"
    );
    let right = attitude(0.0, -s, c);
    assert!(
        near(right.roll, 5.0, 0.01) && near(right.pitch, 0.0, 0.01),
        "{right:?}"
    );
    let upright = attitude(-1.0, 0.0, 0.0);
    assert!(near(upright.pitch, 90.0, 0.01), "{upright:?}");
}

#[test]
fn shocks_and_bad_samples_are_refused() {
    assert_eq!(attitude_from_accel(0.0, 0.0, 0.0), None);
    assert_eq!(attitude_from_accel(0.0, 0.0, 3.0), None);
    assert_eq!(attitude_from_accel(f32::NAN, 0.0, 1.0), None);
    assert_eq!(attitude_from_accel(0.0, f32::INFINITY, 1.0), None);
}

#[test]
fn filter_follows_a_step_without_visible_lag() {
    let mut f = AttitudeFilter::default();
    f.update(Attitude::default(), 10.0);
    let step = Attitude {
        pitch: 10.0,
        roll: -10.0,
    };
    let mut out = Attitude::default();
    let mut ms = 0;
    while out.pitch < 9.0 && ms < 1000 {
        out = f.update(step, 10.0);
        ms += 10;
    }
    assert!(ms <= 70, "90% of a 10 degree step took {ms} ms");
    assert!(near(out.roll, -out.pitch, 1e-4));
    for _ in 0..33 {
        out = f.update(step, 10.0);
    }
    assert!(near(out.pitch, 10.0, 0.1), "{out:?}");
}

#[test]
fn filter_hides_noise_while_still() {
    let mut f = AttitudeFilter::default();
    f.update(Attitude::default(), 10.0);
    let mut worst = 0.0_f32;
    for i in 0..400 {
        let n = if i % 2 == 0 { 0.3 } else { -0.3 };
        let out = f.update(Attitude { pitch: n, roll: -n }, 10.0);
        if i > 50 {
            worst = worst.max(out.pitch.abs()).max(out.roll.abs());
        }
    }
    assert!(worst < 0.05, "residual noise {worst}");
}

#[test]
fn tenths_round_clamp_and_refuse_nan() {
    assert_eq!(tenths(1.24), 12);
    assert_eq!(tenths(-1.26), -13);
    assert_eq!(tenths(-0.04), 0);
    assert_eq!(tenths(500.0), 900);
    assert_eq!(tenths(-500.0), -900);
    assert_eq!(tenths(f32::NAN), 0);
}

#[test]
fn publish_announces_only_a_changed_value() {
    let shared = Shared::default();
    assert!(shared.publish(Attitude {
        pitch: 1.0,
        roll: -2.0
    }));
    assert_eq!(shared.get(), (10, -20));
    assert!(
        !shared.publish(Attitude {
            pitch: 1.02,
            roll: -2.01
        }),
        "same tenths"
    );
    assert!(shared.publish(Attitude {
        pitch: 1.06,
        roll: -2.0
    }));
    assert_eq!(shared.get(), (11, -20));
}

#[test]
fn the_source_answers_attitude_as_a_record_and_refuses_others() {
    let shared = Shared::default();
    shared.publish(Attitude {
        pitch: -3.0,
        roll: 4.5,
    });
    let mut data = LevelData::new(shared);
    let answer = data.query("attitude", &[]);
    assert_eq!(
        answer.ok(),
        Some(Value::record(vec![
            Value::Number(-30.0),
            Value::Number(45.0)
        ]))
    );
    assert!(matches!(
        data.query("weather", &[]),
        Err(DataError::UnknownSource(_))
    ));
}
