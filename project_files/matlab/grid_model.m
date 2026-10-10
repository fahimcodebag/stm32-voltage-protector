%% grid_model.m - synthetic 220 V, 50 Hz grid with fault scenarios
%  Generates ADC-domain test vectors for the STM32 voltage protector and
%  validates the cycle-RMS trip logic offline before it runs on target.
%  Base MATLAB only - no toolboxes required. Written for R2022b.

clear; close all; clc;

%% ---- system parameters (keep in sync with firmware config.h) ----
P.f_grid    = 50;        % Hz, Bangladesh grid
P.V_nom     = 220;       % Vrms nominal
P.fs        = 1000;      % Hz ADC sampling -> 20 samples/cycle
P.cycles    = 60;        % total cycles to synthesise
P.adc_bits  = 12;
P.vref      = 3.3;       % V
P.v_max     = 400;       % Vpk the front end must survive without clipping
P.div_gain  = (P.vref/2) / P.v_max;   % divider scales +/-400 V into +/-1.65 V
P.bias      = P.vref/2;               % mid-rail bias keeps the swing positive

% protection thresholds (Vrms)
P.uv_trip   = 180;
P.ov_trip   = 250;
P.i_trip    = 10;        % A

N  = round(P.fs * P.cycles / P.f_grid);
t  = (0:N-1).'/P.fs;

%% ---- scenario definition: piecewise RMS envelope ----
env = P.V_nom * ones(N,1);
seg = @(a,b) (t >= a) & (t < b);

env(seg(0.30,0.50)) = 160;    % deep sag        -> expect UV trip
env(seg(0.70,0.90)) = 270;    % sustained swell -> expect OV trip
env(seg(1.10,1.14)) = 330;    % fast surge      -> expect OV trip within 1-2 cycles

v_inst = sqrt(2) * env .* sin(2*pi*P.f_grid*t);
v_inst = v_inst + 2.0*randn(N,1);          % measurement noise

%% ---- map to ADC domain ----
v_adc_volts = P.bias + v_inst * P.div_gain;
v_adc_volts = min(max(v_adc_volts, 0), P.vref);            % clamp at rails
counts = round(v_adc_volts / P.vref * (2^P.adc_bits - 1));
counts = min(max(counts, 0), 2^P.adc_bits - 1);

%% ---- reference implementation of the firmware's cycle RMS ----
spc  = round(P.fs / P.f_grid);              % samples per cycle = 20
ncyc = floor(N / spc);
rms_meas = zeros(ncyc,1);
t_cyc    = zeros(ncyc,1);
for k = 1:ncyc
    idx = (k-1)*spc + (1:spc);
    % undo bias and gain exactly as the firmware will
    v = ((counts(idx) / (2^P.adc_bits - 1)) * P.vref - P.bias) / P.div_gain;
    rms_meas(k) = sqrt(mean(v.^2));
    t_cyc(k)    = t(idx(end));
end

trip = (rms_meas < P.uv_trip) | (rms_meas > P.ov_trip);

%% ---- report ----
fprintf('samples=%d  cycles=%d  samples/cycle=%d\n', N, ncyc, spc);
fprintf('RMS range: %.1f .. %.1f V\n', min(rms_meas), max(rms_meas));
first = find(trip, 1);
if ~isempty(first)
    fprintf('first trip at t=%.3f s (cycle %d), RMS=%.1f V\n', ...
            t_cyc(first), first, rms_meas(first));
end

%% ---- export test vectors for the firmware ----
writematrix(counts, 'test_vector_counts.csv');
T = table(t_cyc, rms_meas, double(trip), 'VariableNames', {'t_s','rms_V','trip'});
writetable(T, 'expected_rms.csv');
fprintf('wrote test_vector_counts.csv (%d samples) and expected_rms.csv\n', numel(counts));

%% ---- figure for the design document / test_evidence ----
figure('Position',[100 100 900 600]);
subplot(3,1,1); plot(t, v_inst); grid on;
ylabel('v(t) [V]'); title('Synthetic grid: sag, swell, surge');
subplot(3,1,2); plot(t, counts); grid on;
ylabel('ADC counts'); ylim([0 4095]);
subplot(3,1,3);
plot(t_cyc, rms_meas, 'LineWidth', 1.2); grid on; hold on;
yline(P.uv_trip,'--','UV trip'); yline(P.ov_trip,'--','OV trip');
stairs(t_cyc, trip*300, 'r');
ylabel('RMS [V]'); xlabel('time [s]'); legend('measured RMS','','','trip');
exportgraphics(gcf, 'grid_model_validation.png', 'Resolution', 150);
fprintf('wrote grid_model_validation.png\n');
