#pragma once

// 1. ПОДКЛЮЧАЕМ НАШИ ФАЙЛЫ
#include "MyForm1.h" 
#include "Model.h"
#include "Controller.h"

namespace age {

	using namespace System;
	using namespace System::ComponentModel;
	using namespace System::Collections;
	using namespace System::Windows::Forms;
	using namespace System::Data;
	using namespace System::Drawing;

	/// <summary>
	/// Сводка для MyForm
	/// </summary>
	public ref class MyForm : public System::Windows::Forms::Form
	{
		// 2. ОБЪЯВЛЯЕМ ПЕРЕМЕННЫЕ ЯДРА
	private:
		AgeModel* model;
	private: System::Windows::Forms::Button^ button1;
		   AgeController* controller;

	public:
		MyForm(void)
		{
			InitializeComponent();

			// 3. СОЗДАЕМ ЯДРО ПРИ ЗАПУСКЕ ПРОГРАММЫ
			model = new AgeModel();
			controller = new AgeController(model);
		}

	protected:
		/// <summary>
		/// Освободить все используемые ресурсы.
		/// </summary>
		~MyForm()
		{
			if (components)
			{
				delete components;
			}
		}

	private:
		/// <summary>
		/// Обязательная переменная конструктора.
		/// </summary>
		System::ComponentModel::Container^ components;

#pragma region Windows Form Designer generated code
		/// <summary>
		/// Требуемый метод для поддержки конструктора — не изменяйте 
		/// содержимое этого метода с помощью редактора кода.
		/// </summary>
		void InitializeComponent(void)
		{
			this->button1 = (gcnew System::Windows::Forms::Button());
			this->SuspendLayout();
			// 
			// button1
			// 
			this->button1->Location = System::Drawing::Point(507, 165);
			this->button1->Name = L"button1";
			this->button1->Size = System::Drawing::Size(135, 23);
			this->button1->TabIndex = 0;
			this->button1->Text = L"button1";
			this->button1->UseVisualStyleBackColor = true;
			this->button1->Click += gcnew System::EventHandler(this, &MyForm::button1_Click_1);
			// 
			// MyForm
			// 
			this->AutoScaleDimensions = System::Drawing::SizeF(6, 13);
			this->AutoScaleMode = System::Windows::Forms::AutoScaleMode::Font;
			this->ClientSize = System::Drawing::Size(696, 261);
			this->Controls->Add(this->button1);
			this->Name = L"MyForm";
			this->Text = L"Главное окно";
			this->Load += gcnew System::EventHandler(this, &MyForm::MyForm_Load);
			this->ResumeLayout(false);

		}
#pragma endregion

	private: System::Void MyForm_Load(System::Object^ sender, System::EventArgs^ e) {
	}

		   // 4. ВОТ СЮДА МЫ ВСТАВИЛИ ВАШ КОД ДЛЯ КНОПКИ
	private: System::Void button1_Click(System::Object^ sender, System::EventArgs^ e) {

		// Открываем второе окно (MyForm1) и передаем ему ядро MVC
		MyForm1^ form2 = gcnew MyForm1(model, controller);
		form2->ShowDialog();

		// После того как второе окно закрылось, выводим результаты в Label
		// (Строки конвертируются в String^ автоматически)
		labelResult->Text = "Ваш возраст:\n" +
			"Лет: " + model->getYears() + "\n" +
			"Месяцев: " + model->getMonths() + "\n" +
			"Дней: " + model->getDays() + "\n" +
			"Минут: " + model->getMinutes() + "\n\n" +
			"В неожиданных единицах:\n" +
			"Чашек кофе: " + model->getCoffeeCups() + "\n" +
			"Серий сериала: " + model->getTvSeries() + "\n" +
			"Километров ленты соцсетей: " + model->getSocialMediaKm();
	}

	private: System::Void button1_Click_1(System::Object^ sender, System::EventArgs^ e) {
	}
	};
}