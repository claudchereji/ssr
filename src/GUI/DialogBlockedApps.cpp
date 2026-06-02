/*
Copyright (c) 2012-2020 Maarten Baert <maarten-baert@hotmail.com>

This file is part of SimpleScreenRecorder.

SimpleScreenRecorder is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

SimpleScreenRecorder is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with SimpleScreenRecorder.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "DialogBlockedApps.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QListWidgetItem>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

// Helper to get WM_CLASS from an X11 window
static QString GetWindowClass(Display* dpy, Window win) {
	XClassHint class_hint;
	if(XGetClassHint(dpy, win, &class_hint)) {
		QString name;
		if(class_hint.res_class != NULL) {
			name = QString::fromLatin1(class_hint.res_class);
		}
		if(class_hint.res_name != NULL)
			XFree(class_hint.res_name);
		if(class_hint.res_class != NULL)
			XFree(class_hint.res_class);
		return name;
	}
	return QString();
}

// Recursively collect all window classes from the root window
static std::vector<QString> GetRunningWindowClasses() {
	std::vector<QString> result;
	Display* dpy = XOpenDisplay(NULL);
	if(dpy == NULL)
		return result;

	Window root = DefaultRootWindow(dpy);

	// helper lambda to recursively query windows
	std::function<void(Window)> query = [&](Window win) {
		Window parent, *children = NULL;
		unsigned int nchildren;
		if(XQueryTree(dpy, win, &root, &parent, &children, &nchildren)) {
			for(unsigned int i = 0; i < nchildren; ++i) {
				QString cls = GetWindowClass(dpy, children[i]);
				if(!cls.isEmpty()) {
					// avoid duplicates
					bool found = false;
					for(const QString& existing : result) {
						if(existing == cls) {
							found = true;
							break;
						}
					}
					if(!found)
						result.push_back(cls);
				}
				query(children[i]);
			}
			if(children != NULL)
				XFree(children);
		}
	};

	query(root);
	XCloseDisplay(dpy);
	return result;
}

DialogBlockedApps::DialogBlockedApps(QWidget* parent, const std::vector<QString>& initial_blocked)
	: QDialog(parent) {

	m_blocked_apps = initial_blocked;

	setWindowTitle(tr("Blocked Applications"));
	setMinimumWidth(400);

	QVBoxLayout *layout = new QVBoxLayout(this);

	// running apps list
	QLabel *label_running = new QLabel(tr("Running applications (check to block):"));
	layout->addWidget(label_running);

	m_list_running = new QListWidget(this);
	m_list_running->setToolTip(tr("Select applications to always overlay with 'Private Window'."));
	connect(m_list_running, SIGNAL(itemChanged(QListWidgetItem*)), this, SLOT(OnItemChanged(QListWidgetItem*)));
	layout->addWidget(m_list_running);

	// refresh button
	m_pushbutton_refresh = new QPushButton(tr("Refresh list"), this);
	connect(m_pushbutton_refresh, SIGNAL(clicked()), this, SLOT(OnRefresh()));
	layout->addWidget(m_pushbutton_refresh);

	// manual entry
	QLabel *label_manual = new QLabel(tr("Or enter application class name manually:"));
	layout->addWidget(label_manual);

	{
		QHBoxLayout *layout2 = new QHBoxLayout();
		layout->addLayout(layout2);
		m_lineedit_manual = new QLineEdit(this);
		m_lineedit_manual->setPlaceholderText(tr("e.g. Signal, firefox, code"));
		layout2->addWidget(m_lineedit_manual);
		m_pushbutton_add_manual = new QPushButton(tr("Add"), this);
		connect(m_pushbutton_add_manual, SIGNAL(clicked()), this, SLOT(OnAddManual()));
		layout2->addWidget(m_pushbutton_add_manual);
	}

	// dialog buttons
	{
		QHBoxLayout *layout2 = new QHBoxLayout();
		layout->addLayout(layout2);
		layout2->addStretch();
		QPushButton *ok = new QPushButton(tr("OK"), this);
		connect(ok, SIGNAL(clicked()), this, SLOT(accept()));
		layout2->addWidget(ok);
	}

	OnRefresh();
}

std::vector<QString> DialogBlockedApps::GetBlockedApps() {
	return m_blocked_apps;
}

void DialogBlockedApps::OnRefresh() {
	m_list_running->clear();
	std::vector<QString> classes = GetRunningWindowClasses();
	for(const QString& cls : classes) {
		QListWidgetItem *item = new QListWidgetItem(cls, m_list_running);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		// check if already blocked
		bool blocked = false;
		for(const QString& b : m_blocked_apps) {
			if(b.compare(cls, Qt::CaseInsensitive) == 0) {
				blocked = true;
				break;
			}
		}
		item->setCheckState(blocked ? Qt::Checked : Qt::Unchecked);
	}
}

void DialogBlockedApps::OnAddManual() {
	QString text = m_lineedit_manual->text().trimmed();
	if(text.isEmpty())
		return;
	// avoid duplicates
	bool found = false;
	for(const QString& b : m_blocked_apps) {
		if(b.compare(text, Qt::CaseInsensitive) == 0) {
			found = true;
			break;
		}
	}
	if(!found) {
		m_blocked_apps.push_back(text);
		// add to list if not already there
		bool in_list = false;
		for(int i = 0; i < m_list_running->count(); ++i) {
			if(m_list_running->item(i)->text().compare(text, Qt::CaseInsensitive) == 0) {
				in_list = true;
				m_list_running->item(i)->setCheckState(Qt::Checked);
				break;
			}
		}
		if(!in_list) {
			QListWidgetItem *item = new QListWidgetItem(text, m_list_running);
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(Qt::Checked);
		}
	}
	m_lineedit_manual->clear();
}

void DialogBlockedApps::OnItemChanged(QListWidgetItem* item) {
	QString cls = item->text();
	bool checked = (item->checkState() == Qt::Checked);
	// update m_blocked_apps
	bool found = false;
	for(size_t i = 0; i < m_blocked_apps.size(); ++i) {
		if(m_blocked_apps[i].compare(cls, Qt::CaseInsensitive) == 0) {
			found = true;
			if(!checked) {
				m_blocked_apps.erase(m_blocked_apps.begin() + i);
			}
			break;
		}
	}
	if(checked && !found) {
		m_blocked_apps.push_back(cls);
	}
}
