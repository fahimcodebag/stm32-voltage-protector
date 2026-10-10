%% proteus_stimulus.m - fault-scenario waveforms for the Proteus simulation
%  Writes the conditioned sensor voltages for PA0 (line voltage) and PA1
%  (load current) as time/value text files for Proteus FILE generators, so
%  one simulation run plays back a scripted sequence of grid faults.
%
%  The files model the OUTPUT of the analog front end, not the mains itself:
%    PA0 = 1.65 V + v(t) * 1.65/400   (+/-400 V peak  -> 0..3.3 V)
%    PA1 = 1.65 V + i(t) * 1.65/20    (+/-20 A peak   -> 0..3.3 V)
%  Matching firmware calibration: built in as the defaults of
%  VoltageProtector_ac.elf (build_ac.bat), or send over UART on the DC build:
%    SET VOFF 2048, SET VSPAN 800, SET IOFF 2048, SET ISPAN 40, SET RESTORE 1
%  VSPAN/ISPAN are twice the peak range because the firmware maps a full
%  4095-count deviation to the span, and a mid-rail signal can only deviate
%  by 2047 counts.
%
%  Each event starts more than restore_s (1 s) after the previous one ends:
%  while the relay is open the firmware does not evaluate new faults, so
%  closely spaced events would be masked by the previous trip.
%
%  The current is the prospective load current (v / R_load). The file cannot
%  know when the relay opens, so after a trip PA1 still carries current; only
%  trips while connected are meaningful for over-current.
%  Base MATLAB only. Written for R2022b.

clear; close all; clc;

%% ---- parameters (keep in sync with grid_model.m / firmware) ----
P.f_grid  = 50;          % Hz
P.V_nom   = 220;         % Vrms
P.fs      = 5000;        % Hz output rate: 100 points per mains cycle
P.T_end   = 13.5;        % s (whole cycles, so a repeated file loops cleanly)
P.bias    = 3.3/2;       % V, mid-rail
P.v_gain  = P.bias/400;  % V per volt of mains
P.i_gain  = P.bias/20;   % V per amp of load
P.R_nom   = P.V_nom/5;   % ohm, 5 A rms nominal resistive load
P.R_fault = 15;          % ohm, load fault -> ~14.7 A rms (> 10 A trip)

t   = (0:1/P.fs:P.T_end).';
seg = @(a,b) (t >= a) & (t < b);

%% ---- scenario: RMS envelope and load resistance ----
env = P.V_nom * ones(size(t));
% sag, swell and load fault last 1.5 s so the LCD (which Proteus refreshes
% slowly in real time) shows the live reading during the event; the surge
% stays at 3 cycles as the fast-transient test.
env(seg(1.00,2.50)) = 160;          % sag           -> UV trip (180 V)
env(seg(4.50,6.00)) = 270;          % swell         -> OV trip (250 V)
env(seg(8.00,8.06)) = 330;          % 3-cycle surge -> OV trip
R = P.R_nom * ones(size(t));
R(seg(10.00,11.50)) = P.R_fault;    % load fault    -> OC trip (10 A)

v = sqrt(2) * env .* sin(2*pi*P.f_grid*t);
v = v + 1.0*randn(size(t));         % small measurement noise
i = v ./ R;

pa0 = min(max(P.bias + v*P.v_gain, 0), 3.3);
pa1 = min(max(P.bias + i*P.i_gain, 0), 3.3);

%% ---- write Proteus FILE-generator data: "time value" per line ----
write_pwl('pa0_voltage.txt', t, pa0);
write_pwl('pa1_current.txt', t, pa1);

% short format check: two healthy cycles only
k = t < 0.04;
write_pwl('format_test.txt', t(k), pa0(k));

fprintf('wrote pa0_voltage.txt, pa1_current.txt (%d points each), format_test.txt\n', numel(t));
fprintf('expected: UV trip ~1.0 s, OV ~4.5 s, OV (surge) ~8.0 s, OC ~10.0 s\n');

%% ---- plot for the design document ----
f = figure('Visible','off','Position',[100 100 900 500]);
subplot(2,1,1); plot(t, pa0); ylabel('PA0 (V)'); grid on;
title('Proteus stimulus: line voltage sensor output');
subplot(2,1,2); plot(t, pa1); ylabel('PA1 (V)'); xlabel('t (s)'); grid on;
title('Proteus stimulus: load current sensor output');
exportgraphics(f, 'proteus_stimulus.png', 'Resolution', 150);

function write_pwl(name, t, y)
    fid = fopen(name, 'w');
    fprintf(fid, '%.6f %.5f\n', [t y].');
    fclose(fid);
end
