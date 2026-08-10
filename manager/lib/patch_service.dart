import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';

const repositoryApi =
    'https://api.github.com/repos/silenceremember/prism-family-organizer-patch/releases?per_page=20';

enum LauncherFamily {
  pineconemc('pineconemc', 'PineconeMC'),
  prism('prism', 'Prism Launcher'),
  freesm('freesm', 'Freesm Launcher');

  const LauncherFamily(this.id, this.label);

  final String id;
  final String label;

  static LauncherFamily? fromId(String value) {
    for (final family in values) {
      if (family.id == value.toLowerCase()) return family;
    }
    return null;
  }
}

class ReleaseAsset {
  const ReleaseAsset({
    required this.version,
    required this.url,
    required this.sha256,
    required this.size,
  });

  final String version;
  final Uri url;
  final String sha256;
  final int size;
}

class PatchState {
  const PatchState({
    required this.version,
    required this.family,
    required this.original,
    required this.originalSha256,
    required this.installedSha256,
  });

  factory PatchState.fromJson(Map<String, dynamic> json) => PatchState(
    version: json['version'] as String? ?? '',
    family: json['family'] as String? ?? '',
    original: json['original'] as String? ?? '',
    originalSha256: (json['originalSha256'] as String? ?? '').toLowerCase(),
    installedSha256: (json['installedSha256'] as String? ?? '').toLowerCase(),
  );

  final String version;
  final String family;
  final String original;
  final String originalSha256;
  final String installedSha256;
}

class LauncherContext {
  const LauncherContext({
    required this.target,
    required this.family,
    required this.stateFile,
    required this.state,
  });

  final File target;
  final LauncherFamily family;
  final File stateFile;
  final PatchState? state;

  bool get installed => state != null;
}

class PatchException implements Exception {
  const PatchException(this.message);
  final String message;

  @override
  String toString() => message;
}

typedef DownloadProgress = void Function(int received, int total);

class PatchService {
  PatchService({HttpClient? client}) : _client = client ?? HttpClient();

  final HttpClient _client;

  String join(String first, String second) =>
      '$first${first.endsWith(Platform.pathSeparator) ? '' : Platform.pathSeparator}$second';

  LauncherFamily? detectFamily(String executablePath) {
    final name = executablePath.split(RegExp(r'[/\\]')).last.toLowerCase();
    if (name.contains('freesm')) return LauncherFamily.freesm;
    if (name.contains('elyprism') || name.contains('pinecone')) {
      return LauncherFamily.pineconemc;
    }
    if (name.contains('prism')) return LauncherFamily.prism;
    return null;
  }

  Future<LauncherContext> inspect(
    String targetPath, {
    LauncherFamily? familyHint,
  }) async {
    final target = File(targetPath);
    if (!await target.exists()) {
      throw const PatchException('Select an existing launcher executable.');
    }
    final family = familyHint ?? detectFamily(target.path);
    if (family == null) {
      throw const PatchException(
        'This executable is not a supported Prism-family launcher.',
      );
    }
    final stateFile = File(
      join(join(target.parent.path, '.organizer-patch'), 'state.json'),
    );
    PatchState? state;
    if (await stateFile.exists()) {
      try {
        final value = jsonDecode(await stateFile.readAsString());
        if (value is! Map<String, dynamic>) throw const FormatException();
        state = PatchState.fromJson(value);
      } on Object {
        throw const PatchException('The installed patch state is invalid.');
      }
      if (state.family.isNotEmpty && state.family != family.id) {
        throw const PatchException(
          'The saved patch state belongs to another launcher family.',
        );
      }
    }
    return LauncherContext(
      target: target,
      family: family,
      stateFile: stateFile,
      state: state,
    );
  }

  Future<ReleaseAsset> latest(LauncherFamily family) async {
    final request = await _client.getUrl(Uri.parse(repositoryApi));
    request.headers
      ..set(HttpHeaders.userAgentHeader, 'Prism-Family-Organizer-Patch-Manager')
      ..set(HttpHeaders.acceptHeader, 'application/vnd.github+json');
    final response = await request.close();
    final body = await utf8.decoder.bind(response).join();
    if (response.statusCode != HttpStatus.ok) {
      throw PatchException('GitHub check failed (${response.statusCode}).');
    }
    final decoded = jsonDecode(body);
    if (decoded is! List) {
      throw const PatchException('GitHub returned invalid release data.');
    }
    final expected =
        'prism-family-organizer-patch-${family.id}-windows-x64.exe';
    for (final releaseValue in decoded) {
      if (releaseValue is! Map<String, dynamic> ||
          releaseValue['draft'] == true) {
        continue;
      }
      var version = releaseValue['tag_name'] as String? ?? '';
      if (version.toLowerCase().startsWith('v')) version = version.substring(1);
      final assets = releaseValue['assets'];
      if (version.isEmpty || assets is! List) continue;
      for (final assetValue in assets) {
        if (assetValue is! Map<String, dynamic> ||
            assetValue['name'] != expected) {
          continue;
        }
        final digestValue = assetValue['digest'] as String? ?? '';
        if (!digestValue.toLowerCase().startsWith('sha256:')) continue;
        final url = Uri.tryParse(
          assetValue['browser_download_url'] as String? ?? '',
        );
        final digest = digestValue.substring(7).toLowerCase();
        if (url == null || digest.length != 64) continue;
        return ReleaseAsset(
          version: version,
          url: url,
          sha256: digest,
          size: assetValue['size'] as int? ?? -1,
        );
      }
    }
    throw PatchException(
      'No compatible Windows release was found for ${family.label}.',
    );
  }

  Future<File> download(
    ReleaseAsset asset,
    Directory directory,
    DownloadProgress onProgress,
  ) async {
    await directory.create(recursive: true);
    final destination = File(
      join(directory.path, 'organizer-${asset.version}.exe'),
    );
    final request = await _client.getUrl(asset.url);
    request.headers.set(
      HttpHeaders.userAgentHeader,
      'Prism-Family-Organizer-Patch-Manager',
    );
    final response = await request.close();
    if (response.statusCode != HttpStatus.ok) {
      throw PatchException('Download failed (${response.statusCode}).');
    }
    final total = response.contentLength > 0
        ? response.contentLength
        : asset.size;
    var received = 0;
    final sink = destination.openWrite();
    try {
      await for (final chunk in response) {
        sink.add(chunk);
        received += chunk.length;
        onProgress(received, total);
      }
      await sink.flush();
    } finally {
      await sink.close();
    }
    final actual = await sha256File(destination);
    if (actual != asset.sha256) {
      await destination.delete().catchError((_) => destination);
      throw const PatchException(
        'Downloaded launcher failed SHA-256 verification.',
      );
    }
    return destination;
  }

  Future<String> sha256File(File file) async =>
      (await sha256.bind(file.openRead()).first).toString();

  Future<void> install(
    LauncherContext context,
    ReleaseAsset asset,
    File source,
  ) async {
    if (context.installed) {
      throw const PatchException(
        'The patch is already installed. Use Reinstall.',
      );
    }
    await _replaceLauncher(
      context,
      source: source,
      expectedHash: asset.sha256,
      mode: _PatchMode.install,
      version: asset.version,
    );
  }

  Future<void> reinstall(
    LauncherContext context,
    ReleaseAsset asset,
    File source,
  ) async {
    if (!context.installed) {
      throw const PatchException('Install the patch first.');
    }
    await _replaceLauncher(
      context,
      source: source,
      expectedHash: asset.sha256,
      mode: _PatchMode.update,
      version: asset.version,
    );
  }

  Future<void> remove(LauncherContext context) async {
    final state = context.state;
    if (state == null) {
      throw const PatchException('Organizer Patch is not installed.');
    }
    final relative = state.original.replaceAll('/', Platform.pathSeparator);
    final original = File(join(context.stateFile.parent.path, relative));
    if (!await original.exists() || state.originalSha256.length != 64) {
      throw const PatchException('The saved original launcher is missing.');
    }
    if (await sha256File(original) != state.originalSha256) {
      throw const PatchException(
        'The saved original launcher failed SHA-256 verification.',
      );
    }
    await _replaceLauncher(
      context,
      source: original,
      expectedHash: state.originalSha256,
      mode: _PatchMode.remove,
    );
  }

  Future<void> _replaceLauncher(
    LauncherContext context, {
    required File source,
    required String expectedHash,
    required _PatchMode mode,
    String version = '',
  }) async {
    final target = context.target;
    final stateFile = context.stateFile;
    if (!_validStateLocation(stateFile, target) ||
        _samePath(source.path, target.path)) {
      throw const PatchException('Unsafe Organizer maintenance paths.');
    }
    if (expectedHash.length != 64 ||
        await sha256File(source) != expectedHash.toLowerCase()) {
      throw const PatchException(
        'Source executable failed SHA-256 verification.',
      );
    }

    File? installOriginal;
    var installOriginalHash = '';
    if (mode == _PatchMode.install) {
      if (version.isEmpty || await stateFile.exists()) {
        throw const PatchException(
          'Organizer Patch is already installed or has no version.',
        );
      }
      installOriginal = File(
        join(
          join(stateFile.parent.path, 'original'),
          target.uri.pathSegments.last,
        ),
      );
      if (await installOriginal.exists()) {
        throw const PatchException(
          'A pristine launcher snapshot already exists.',
        );
      }
      await installOriginal.parent.create(recursive: true);
      await target.copy(installOriginal.path);
      installOriginalHash = await sha256File(installOriginal);
      if (installOriginalHash.length != 64) {
        await installOriginal.delete().catchError((_) => installOriginal!);
        throw const PatchException(
          'Could not verify the pristine launcher executable.',
        );
      }
    }

    final rollback = File('${target.path}.organizer-rollback');
    if (await rollback.exists()) await rollback.delete();
    try {
      await _renameWithRetry(target, rollback);
    } on Object {
      await _cleanInstallSnapshot(stateFile, installOriginal);
      rethrow;
    }

    Future<void> restoreRollback() async {
      if (await target.exists()) await target.delete();
      if (await rollback.exists()) await rollback.rename(target.path);
    }

    try {
      await source.copy(target.path);
      if (await sha256File(target) != expectedHash.toLowerCase()) {
        throw const PatchException(
          'Installed executable failed SHA-256 verification.',
        );
      }

      if (mode == _PatchMode.install) {
        await _saveJsonAtomically(stateFile, {
          'schema': 1,
          'version': version,
          'family': context.family.id,
          'executable': target.uri.pathSegments.last,
          'original': 'original/${installOriginal!.uri.pathSegments.last}',
          'originalSha256': installOriginalHash,
          'installedSha256': expectedHash.toLowerCase(),
          'updatedAt': DateTime.now().toUtc().toIso8601String(),
        });
      } else if (mode == _PatchMode.update) {
        final value = jsonDecode(await stateFile.readAsString());
        if (value is! Map<String, dynamic> || version.isEmpty) {
          throw const PatchException('The installed patch state is invalid.');
        }
        value
          ..['version'] = version
          ..['installedSha256'] = expectedHash.toLowerCase()
          ..['updatedAt'] = DateTime.now().toUtc().toIso8601String();
        await _saveJsonAtomically(stateFile, value);
      } else {
        await stateFile.parent.delete(recursive: true);
      }
    } on Object {
      await restoreRollback();
      await _cleanInstallSnapshot(stateFile, installOriginal);
      rethrow;
    }

    await _deleteWithRetry(rollback);
    if (mode == _PatchMode.update && await source.exists()) {
      await source.delete();
    }
  }

  bool _validStateLocation(File state, File target) =>
      _baseName(state.path).toLowerCase() == 'state.json' &&
      _baseName(state.parent.path).toLowerCase() == '.organizer-patch' &&
      _samePath(state.parent.parent.path, target.parent.path);

  String _baseName(String path) => path.split(RegExp(r'[/\\]')).last;

  bool _samePath(String first, String second) =>
      File(first).absolute.path.toLowerCase() ==
      File(second).absolute.path.toLowerCase();

  Future<void> _renameWithRetry(File source, File destination) async {
    for (var attempt = 0; attempt < 300; attempt++) {
      try {
        await source.rename(destination.path);
        return;
      } on FileSystemException {
        await Future<void>.delayed(const Duration(milliseconds: 100));
      }
    }
    throw const PatchException('Timed out waiting for the launcher to close.');
  }

  Future<void> _deleteWithRetry(File file) async {
    for (var attempt = 0; attempt < 300; attempt++) {
      if (!await file.exists()) return;
      try {
        await file.delete();
        return;
      } on FileSystemException {
        await Future<void>.delayed(const Duration(milliseconds: 100));
      }
    }
    throw const PatchException('Could not remove the rollback file.');
  }

  Future<void> _saveJsonAtomically(
    File destination,
    Map<String, dynamic> value,
  ) async {
    await destination.parent.create(recursive: true);
    final temporary = File(
      '${destination.path}.tmp-$pid-${DateTime.now().microsecondsSinceEpoch}',
    );
    final backup = File('${destination.path}.previous');
    await temporary.writeAsString(
      const JsonEncoder.withIndent('  ').convert(value),
      flush: true,
    );
    if (await backup.exists()) await backup.delete();
    final hadPrevious = await destination.exists();
    if (hadPrevious) await destination.rename(backup.path);
    try {
      await temporary.rename(destination.path);
      if (await backup.exists()) await backup.delete();
    } on Object {
      if (await destination.exists()) await destination.delete();
      if (await backup.exists()) await backup.rename(destination.path);
      rethrow;
    } finally {
      if (await temporary.exists()) await temporary.delete();
    }
  }

  Future<void> _cleanInstallSnapshot(File stateFile, File? original) async {
    if (original == null) return;
    if (await stateFile.exists()) await stateFile.delete();
    if (await original.exists()) await original.delete();
    if (await original.parent.exists()) await original.parent.delete();
    if (await stateFile.parent.exists()) await stateFile.parent.delete();
  }

  void close() => _client.close(force: true);
}

enum _PatchMode { install, update, remove }
