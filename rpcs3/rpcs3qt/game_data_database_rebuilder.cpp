#include "stdafx.h"
#include "game_data_database_rebuilder.h"

#include "progress_dialog.h"

#include <QApplication>
#include <QMessageBox>
#include <QtConcurrent/QtConcurrentRun>

#include <atomic>

std::optional<rpcs3::utils::game_data_database_result> game_data_database_rebuilder::run(QWidget* parent, const std::map<std::string, std::string>& games_yml_entries)
{
	std::atomic<u32> progress_value{0};
	std::atomic<u32> progress_max{1};
	std::atomic<bool> canceled{false};

	progress_dialog pdlg(QObject::tr("Rebuild Game Data Database"), QObject::tr("Scanning game data and rebuilding index references..."), QObject::tr("Cancel"), 0, 1, false, parent, Qt::Dialog | Qt::WindowTitleHint | Qt::CustomizeWindowHint);
	pdlg.setAutoClose(false);
	pdlg.setAutoReset(false);
	pdlg.show();

	QFuture<rpcs3::utils::game_data_database_result> future = QtConcurrent::run([&]()
	{
		return rpcs3::utils::rebuild_game_data_database(games_yml_entries,
			[&](u32 current, u32 total)
			{
				progress_max = std::max<u32>(total, 1);
				progress_value = std::min(current, progress_max.load());
			},
			[&]()
			{
				return canceled.load();
			});
	});

	while (!future.isFinished())
	{
		QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
		pdlg.SetRange(0, static_cast<int>(progress_max.load()));
		pdlg.SetValue(static_cast<int>(progress_value.load()));

		if (pdlg.wasCanceled())
		{
			canceled = true;
		}
	}

	pdlg.SetValue(pdlg.maximum());
	pdlg.accept();

	if (pdlg.wasCanceled())
	{
		QMessageBox::information(parent, QObject::tr("Rebuild Game Data Database"), QObject::tr("The game data database rebuild was canceled."));
		return std::nullopt;
	}

	return future.result();
}
