import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:organizer_patch_manager/patch_service.dart';

void main() {
  test('detects every supported launcher family', () {
    final service = PatchService();
    addTearDown(service.close);

    expect(
      service.detectFamily(r'C:\Portable\elyprismlauncher.exe'),
      LauncherFamily.pineconemc,
    );
    expect(
      service.detectFamily(r'C:\Portable\prismlauncher.exe'),
      LauncherFamily.prism,
    );
    expect(
      service.detectFamily(r'C:\Portable\freesmlauncher.exe'),
      LauncherFamily.freesm,
    );
    expect(service.detectFamily(r'C:\Portable\unrelated.exe'), isNull);
  });

  test('family ids are stable release asset keys', () {
    expect(LauncherFamily.fromId('pineconemc'), LauncherFamily.pineconemc);
    expect(LauncherFamily.fromId('PRISM'), LauncherFamily.prism);
    expect(LauncherFamily.fromId('freesm'), LauncherFamily.freesm);
    expect(LauncherFamily.fromId('unknown'), isNull);
  });

  test(
    'install, reinstall, and remove restore the pristine launcher',
    () async {
      final service = PatchService();
      final fixture = await Directory.systemTemp.createTemp(
        'organizer-manager-test-',
      );
      addTearDown(() async {
        service.close();
        if (await fixture.exists()) await fixture.delete(recursive: true);
      });

      final target = File(
        '${fixture.path}${Platform.pathSeparator}elyprismlauncher.exe',
      );
      await target.writeAsString('pristine-launcher', flush: true);
      final pristine = await target.readAsBytes();
      var context = await service.inspect(
        target.path,
        familyHint: LauncherFamily.pineconemc,
      );

      final first = File('${fixture.path}${Platform.pathSeparator}first.exe');
      await first.writeAsString('organizer-test-6', flush: true);
      final firstAsset = ReleaseAsset(
        version: '0.1.0-test.6',
        url: Uri.parse('https://example.invalid/first.exe'),
        sha256: await service.sha256File(first),
        size: await first.length(),
      );
      await service.install(context, firstAsset, first);
      context = await service.inspect(
        target.path,
        familyHint: LauncherFamily.pineconemc,
      );
      expect(context.state?.version, '0.1.0-test.6');
      expect(await target.readAsString(), 'organizer-test-6');

      final second = File('${fixture.path}${Platform.pathSeparator}second.exe');
      await second.writeAsString('organizer-test-7', flush: true);
      final secondAsset = ReleaseAsset(
        version: '0.1.0-test.7',
        url: Uri.parse('https://example.invalid/second.exe'),
        sha256: await service.sha256File(second),
        size: await second.length(),
      );
      await service.reinstall(context, secondAsset, second);
      context = await service.inspect(
        target.path,
        familyHint: LauncherFamily.pineconemc,
      );
      expect(context.state?.version, '0.1.0-test.7');
      expect(await target.readAsString(), 'organizer-test-7');

      await service.remove(context);
      expect(await target.readAsBytes(), pristine);
      expect(await context.stateFile.exists(), isFalse);
    },
  );
}
