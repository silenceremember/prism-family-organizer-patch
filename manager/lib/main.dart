import 'dart:io';

import 'package:file_selector/file_selector.dart';
import 'package:flutter/material.dart';
import 'package:flutter_svg/flutter_svg.dart';
import 'package:window_manager/window_manager.dart';

import 'patch_service.dart';

const _background = Color(0xFF0B0B0B);
const _surface = Color(0xFF121212);
const _surfaceRaised = Color(0xFF181818);
const _border = Color(0xFF2B2B2B);
const _muted = Color(0xFF929292);
const _text = Color(0xFFF2F2F2);

Future<void> main(List<String> arguments) async {
  WidgetsFlutterBinding.ensureInitialized();
  await windowManager.ensureInitialized();
  const options = WindowOptions(
    size: Size(760, 610),
    minimumSize: Size(680, 560),
    center: true,
    backgroundColor: _background,
    title: 'Organizer Patch Manager',
    titleBarStyle: TitleBarStyle.normal,
  );
  await windowManager.waitUntilReadyToShow(options, () async {
    await windowManager.show();
    await windowManager.focus();
  });
  runApp(OrganizerManagerApp(arguments: arguments));
}

class OrganizerManagerApp extends StatelessWidget {
  const OrganizerManagerApp({required this.arguments, super.key});

  final List<String> arguments;

  @override
  Widget build(BuildContext context) {
    final colorScheme = ColorScheme.fromSeed(
      seedColor: const Color(0xFF9A9A9A),
      brightness: Brightness.dark,
      surface: _surface,
    );
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      title: 'Organizer Patch Manager',
      theme: ThemeData(
        useMaterial3: true,
        brightness: Brightness.dark,
        colorScheme: colorScheme.copyWith(
          surface: _surface,
          onSurface: _text,
          outline: _border,
          primary: const Color(0xFFB8B8B8),
          onPrimary: const Color(0xFF111111),
        ),
        scaffoldBackgroundColor: _background,
        fontFamily: 'Segoe UI',
        dividerColor: _border,
        inputDecorationTheme: const InputDecorationTheme(
          filled: true,
          fillColor: _background,
          border: OutlineInputBorder(borderSide: BorderSide(color: _border)),
          enabledBorder: OutlineInputBorder(
            borderSide: BorderSide(color: _border),
          ),
          focusedBorder: OutlineInputBorder(
            borderSide: BorderSide(color: Color(0xFF7E7E7E)),
          ),
          contentPadding: EdgeInsets.symmetric(horizontal: 14, vertical: 13),
        ),
        snackBarTheme: const SnackBarThemeData(
          backgroundColor: _surfaceRaised,
          contentTextStyle: TextStyle(color: _text),
        ),
      ),
      home: OrganizerManagerPage(arguments: arguments),
    );
  }
}

class OrganizerManagerPage extends StatefulWidget {
  const OrganizerManagerPage({required this.arguments, super.key});

  final List<String> arguments;

  @override
  State<OrganizerManagerPage> createState() => _OrganizerManagerPageState();
}

class _OrganizerManagerPageState extends State<OrganizerManagerPage>
    with WindowListener {
  final _service = PatchService();
  final _targetController = TextEditingController();

  LauncherFamily? _familyHint;
  LauncherContext? _context;
  ReleaseAsset? _latest;
  bool _busy = false;
  bool _restartOnExit = false;
  bool _operationCompleted = false;
  String _status = 'Select a Prism-family launcher to begin.';
  int _received = 0;
  int _total = 0;

  @override
  void initState() {
    super.initState();
    windowManager.addListener(this);
    windowManager.setPreventClose(true);
    final parsed = _parseArguments(widget.arguments);
    _targetController.text = parsed.target ?? _discoverTarget() ?? '';
    _familyHint = parsed.family;
    _restartOnExit = parsed.restartOnExit;
    if (_targetController.text.isNotEmpty) {
      WidgetsBinding.instance.addPostFrameCallback((_) => _inspectAndCheck());
    }
  }

  @override
  void dispose() {
    windowManager.removeListener(this);
    _service.close();
    _targetController.dispose();
    super.dispose();
  }

  @override
  Future<void> onWindowClose() async {
    if (_busy) {
      final close = await _confirm(
        'Close manager?',
        'An operation is still running. Closing now may leave a downloaded temporary file behind.',
        destructive: true,
      );
      if (!close) return;
    }
    if (_restartOnExit && !_operationCompleted) await _startLauncher();
    await windowManager.destroy();
  }

  Future<void> _browse() async {
    const type = XTypeGroup(
      label: 'Prism-family launcher',
      extensions: ['exe'],
    );
    final file = await openFile(acceptedTypeGroups: [type]);
    if (file == null) return;
    _targetController.text = file.path;
    _familyHint = null;
    await _inspectAndCheck();
  }

  Future<void> _inspectAndCheck() async {
    if (_busy || _targetController.text.trim().isEmpty) return;
    setState(() {
      _busy = true;
      _context = null;
      _latest = null;
      _status = 'Inspecting launcher…';
      _received = 0;
      _total = 0;
    });
    try {
      final context = await _service.inspect(
        _targetController.text.trim(),
        familyHint: _familyHint,
      );
      if (!mounted) return;
      setState(() {
        _context = context;
        _status = 'Checking GitHub Releases…';
      });
      final latest = await _service.latest(context.family);
      if (!mounted) return;
      setState(() {
        _latest = latest;
        _status = context.installed
            ? (context.state!.version == latest.version
                  ? 'Organizer Patch is installed and up to date.'
                  : '${latest.version} is available for ${context.family.label}.')
            : 'Ready to install ${latest.version} for ${context.family.label}.';
      });
    } on Object catch (error) {
      if (!mounted) return;
      setState(() => _status = _message(error));
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _install() async {
    final context = _context;
    final latest = _latest;
    if (context == null || latest == null || context.installed) return;
    if (!await _confirm(
      'Install Organizer Patch?',
      'Install ${latest.version} into ${context.family.label}?',
    )) {
      return;
    }
    await _downloadAndApply(
      'Installing',
      (source) => _service.install(context, latest, source),
    );
  }

  Future<void> _reinstall() async {
    final context = _context;
    final latest = _latest;
    if (context == null || latest == null || !context.installed) return;
    if (!await _confirm(
      'Reinstall Organizer Patch?',
      'Download a clean ${latest.version} payload and replace the current patched launcher?',
    )) {
      return;
    }
    await _downloadAndApply(
      'Reinstalling',
      (source) => _service.reinstall(context, latest, source),
    );
  }

  Future<void> _update() async {
    final context = _context;
    final latest = _latest;
    if (context == null ||
        latest == null ||
        !context.installed ||
        context.state!.version == latest.version) {
      return;
    }
    if (!await _confirm(
      'Update Organizer Patch?',
      'Update ${context.state!.version} to ${latest.version}?',
    )) {
      return;
    }
    await _downloadAndApply(
      'Updating',
      (source) => _service.reinstall(context, latest, source),
    );
  }

  Future<void> _remove() async {
    final context = _context;
    if (context == null || !context.installed) return;
    if (!await _confirm(
      'Remove Organizer Patch?',
      'Restore the pristine launcher? Instances and group configuration will be kept.',
      destructive: true,
    )) {
      return;
    }
    setState(() {
      _busy = true;
      _status = 'Restoring the pristine launcher…';
    });
    try {
      await _service.remove(context);
      await _finishAndRestart('Organizer Patch was removed.');
    } on Object catch (error) {
      if (mounted) setState(() => _status = _message(error));
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _downloadAndApply(
    String verb,
    Future<void> Function(File source) apply,
  ) async {
    final context = _context!;
    final latest = _latest!;
    setState(() {
      _busy = true;
      _status = 'Downloading ${latest.version}…';
      _received = 0;
      _total = latest.size;
    });
    try {
      final directory = Directory(
        _service.join(context.target.parent.path, '.organizer-patch/downloads'),
      );
      final source = await _service.download(latest, directory, (
        received,
        total,
      ) {
        if (!mounted) return;
        setState(() {
          _received = received;
          _total = total;
        });
      });
      if (!mounted) return;
      setState(() => _status = '$verb ${latest.version}…');
      await apply(source);
      if (await source.exists()) await source.delete();
      await _finishAndRestart('Organizer Patch ${latest.version} is ready.');
    } on Object catch (error) {
      if (mounted) setState(() => _status = _message(error));
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _finishAndRestart(String message) async {
    if (!mounted) return;
    setState(() => _status = '$message Restarting launcher…');
    await _startLauncher();
    _operationCompleted = true;
    _restartOnExit = false;
    await windowManager.destroy();
  }

  Future<void> _startLauncher() async {
    final path = _context?.target.path ?? _targetController.text.trim();
    if (path.isEmpty || !await File(path).exists()) return;
    await Process.start(
      path,
      const [],
      workingDirectory: File(path).parent.path,
      mode: ProcessStartMode.detached,
    );
  }

  Future<bool> _confirm(
    String title,
    String message, {
    bool destructive = false,
  }) async {
    return await showDialog<bool>(
          context: context,
          builder: (context) => AlertDialog(
            backgroundColor: _surfaceRaised,
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(16),
              side: const BorderSide(color: _border),
            ),
            title: Text(title),
            content: Text(
              message,
              style: const TextStyle(color: _muted, height: 1.45),
            ),
            actions: [
              TextButton(
                onPressed: () => Navigator.pop(context, false),
                child: const Text('Cancel'),
              ),
              FilledButton(
                style: destructive
                    ? FilledButton.styleFrom(
                        backgroundColor: const Color(0xFF5A3030),
                        foregroundColor: _text,
                      )
                    : null,
                onPressed: () => Navigator.pop(context, true),
                child: Text(destructive ? 'Continue' : 'Confirm'),
              ),
            ],
          ),
        ) ??
        false;
  }

  @override
  Widget build(BuildContext context) {
    final installed = _context?.installed ?? false;
    final currentVersion = _context?.state?.version ?? 'Not installed';
    final latestVersion = _latest?.version ?? '—';
    final updateAvailable =
        installed && _latest != null && currentVersion != latestVersion;
    final progress = _total > 0 ? (_received / _total).clamp(0.0, 1.0) : null;

    return Scaffold(
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.fromLTRB(28, 24, 28, 26),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              _BrandHeader(family: _context?.family.label),
              const SizedBox(height: 22),
              _Panel(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    const Text(
                      'LAUNCHER',
                      style: TextStyle(
                        color: _muted,
                        fontSize: 11,
                        fontWeight: FontWeight.w700,
                        letterSpacing: 1.4,
                      ),
                    ),
                    const SizedBox(height: 10),
                    Row(
                      children: [
                        Expanded(
                          child: TextField(
                            controller: _targetController,
                            enabled: !_busy,
                            style: const TextStyle(fontSize: 13),
                            decoration: const InputDecoration(
                              hintText: 'Select launcher executable…',
                            ),
                            onSubmitted: (_) => _inspectAndCheck(),
                          ),
                        ),
                        const SizedBox(width: 10),
                        IconButton.filledTonal(
                          tooltip: 'Browse',
                          onPressed: _busy ? null : _browse,
                          icon: const Icon(Icons.folder_open_rounded),
                        ),
                        const SizedBox(width: 6),
                        IconButton.filledTonal(
                          tooltip: 'Check',
                          onPressed: _busy ? null : _inspectAndCheck,
                          icon: const Icon(Icons.refresh_rounded),
                        ),
                      ],
                    ),
                    const SizedBox(height: 16),
                    Row(
                      children: [
                        Expanded(
                          child: _VersionTile(
                            label: 'INSTALLED',
                            value: currentVersion,
                            icon: installed
                                ? Icons.verified_rounded
                                : Icons.remove_circle_outline_rounded,
                          ),
                        ),
                        const SizedBox(width: 10),
                        Expanded(
                          child: _VersionTile(
                            label: 'LATEST',
                            value: latestVersion,
                            icon: Icons.cloud_done_rounded,
                          ),
                        ),
                      ],
                    ),
                  ],
                ),
              ),
              const SizedBox(height: 12),
              _Panel(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Row(
                      children: [
                        Icon(
                          _busy
                              ? Icons.sync_rounded
                              : Icons.info_outline_rounded,
                          size: 20,
                          color: _muted,
                        ),
                        const SizedBox(width: 10),
                        Expanded(
                          child: Text(
                            _status,
                            style: const TextStyle(fontSize: 13, height: 1.35),
                          ),
                        ),
                      ],
                    ),
                    if (_busy && (_received > 0 || _total != 0)) ...[
                      const SizedBox(height: 14),
                      LinearProgressIndicator(
                        value: progress,
                        minHeight: 7,
                        borderRadius: BorderRadius.circular(20),
                        backgroundColor: _border,
                      ),
                      const SizedBox(height: 7),
                      Text(
                        '${_formatBytes(_received)} / ${_total > 0 ? _formatBytes(_total) : 'unknown'}',
                        style: const TextStyle(color: _muted, fontSize: 11),
                      ),
                    ],
                  ],
                ),
              ),
              const Spacer(),
              GridView.count(
                shrinkWrap: true,
                physics: const NeverScrollableScrollPhysics(),
                crossAxisCount: 4,
                childAspectRatio: 2.15,
                crossAxisSpacing: 10,
                children: [
                  _ActionButton(
                    icon: Icons.download_rounded,
                    label: 'Install',
                    onPressed: !_busy && _latest != null && !installed
                        ? _install
                        : null,
                  ),
                  _ActionButton(
                    icon: Icons.replay_rounded,
                    label: 'Reinstall',
                    onPressed: !_busy && _latest != null && installed
                        ? _reinstall
                        : null,
                  ),
                  _ActionButton(
                    icon: Icons.system_update_alt_rounded,
                    label: 'Update',
                    onPressed: !_busy && updateAvailable ? _update : null,
                  ),
                  _ActionButton(
                    icon: Icons.delete_outline_rounded,
                    label: 'Remove',
                    destructive: true,
                    onPressed: !_busy && installed ? _remove : null,
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }

  String _message(Object error) =>
      error is PatchException ? error.message : 'Unexpected error: $error';

  String? _discoverTarget() {
    final directory = Directory.current;
    for (final name in const [
      'elyprismlauncher.exe',
      'prismlauncher.exe',
      'freesmlauncher.exe',
    ]) {
      final candidate = File(_service.join(directory.path, name));
      if (candidate.existsSync()) return candidate.path;
    }
    return null;
  }

  _LaunchArguments _parseArguments(List<String> values) {
    String? target;
    LauncherFamily? family;
    var restart = false;
    for (var index = 0; index < values.length; index++) {
      switch (values[index]) {
        case '--target' when index + 1 < values.length:
          target = values[++index];
        case '--family' when index + 1 < values.length:
          family = LauncherFamily.fromId(values[++index]);
        case '--restart-on-exit':
          restart = true;
      }
    }
    return _LaunchArguments(
      target: target,
      family: family,
      restartOnExit: restart,
    );
  }
}

class _BrandHeader extends StatelessWidget {
  const _BrandHeader({this.family});
  final String? family;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Container(
          width: 62,
          height: 62,
          padding: const EdgeInsets.all(8),
          decoration: BoxDecoration(
            color: _surface,
            border: Border.all(color: _border),
            borderRadius: BorderRadius.circular(16),
          ),
          child: SvgPicture.asset('assets/Logo_background.svg'),
        ),
        const SizedBox(width: 16),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              SizedBox(
                width: 278,
                height: 46,
                child: SvgPicture.asset(
                  'assets/Logo.svg',
                  alignment: Alignment.centerLeft,
                ),
              ),
              Text(
                family ?? 'Prism family installer & updater',
                style: const TextStyle(color: _muted, fontSize: 12),
              ),
            ],
          ),
        ),
        Container(
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
          decoration: BoxDecoration(
            color: _surfaceRaised,
            border: Border.all(color: _border),
            borderRadius: BorderRadius.circular(30),
          ),
          child: const Text(
            'TRUE DARK',
            style: TextStyle(
              fontSize: 10,
              letterSpacing: 1.2,
              color: Color(0xFFB8B8B8),
              fontWeight: FontWeight.w700,
            ),
          ),
        ),
      ],
    );
  }
}

class _Panel extends StatelessWidget {
  const _Panel({required this.child});
  final Widget child;

  @override
  Widget build(BuildContext context) => Container(
    padding: const EdgeInsets.all(16),
    decoration: BoxDecoration(
      color: _surface,
      border: Border.all(color: _border),
      borderRadius: BorderRadius.circular(14),
    ),
    child: child,
  );
}

class _VersionTile extends StatelessWidget {
  const _VersionTile({
    required this.label,
    required this.value,
    required this.icon,
  });
  final String label;
  final String value;
  final IconData icon;

  @override
  Widget build(BuildContext context) => Container(
    padding: const EdgeInsets.symmetric(horizontal: 13, vertical: 12),
    decoration: BoxDecoration(
      color: _background,
      border: Border.all(color: _border),
      borderRadius: BorderRadius.circular(10),
    ),
    child: Row(
      children: [
        Icon(icon, size: 19, color: const Color(0xFFAAAAAA)),
        const SizedBox(width: 10),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(
                label,
                style: const TextStyle(
                  color: _muted,
                  fontSize: 9,
                  fontWeight: FontWeight.w700,
                  letterSpacing: 1.2,
                ),
              ),
              const SizedBox(height: 3),
              Text(
                value,
                overflow: TextOverflow.ellipsis,
                style: const TextStyle(
                  fontSize: 13,
                  fontWeight: FontWeight.w600,
                ),
              ),
            ],
          ),
        ),
      ],
    ),
  );
}

class _ActionButton extends StatelessWidget {
  const _ActionButton({
    required this.icon,
    required this.label,
    required this.onPressed,
    this.destructive = false,
  });
  final IconData icon;
  final String label;
  final VoidCallback? onPressed;
  final bool destructive;

  @override
  Widget build(BuildContext context) => FilledButton.tonalIcon(
    style: FilledButton.styleFrom(
      backgroundColor: destructive ? const Color(0xFF211515) : _surfaceRaised,
      foregroundColor: destructive ? const Color(0xFFE0A9A9) : _text,
      disabledBackgroundColor: const Color(0xFF101010),
      disabledForegroundColor: const Color(0xFF555555),
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(11),
        side: const BorderSide(color: _border),
      ),
    ),
    onPressed: onPressed,
    icon: Icon(icon, size: 20),
    label: Text(label, style: const TextStyle(fontWeight: FontWeight.w600)),
  );
}

class _LaunchArguments {
  const _LaunchArguments({
    required this.target,
    required this.family,
    required this.restartOnExit,
  });
  final String? target;
  final LauncherFamily? family;
  final bool restartOnExit;
}

String _formatBytes(int bytes) {
  if (bytes < 1024) return '$bytes B';
  const units = ['KiB', 'MiB', 'GiB'];
  var value = bytes / 1024;
  var unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  return '${value < 10 ? value.toStringAsFixed(1) : value.toStringAsFixed(0)} ${units[unit]}';
}
