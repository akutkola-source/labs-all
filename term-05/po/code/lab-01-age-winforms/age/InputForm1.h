#pragma once
#include "Model.h"
#include "Controller.h"
#include <msclr\marshal_cppstd.h>

namespace age {

	using namespace System;
	using namespace System::ComponentModel;
	using namespace System::Collections;
	using namespace System::Windows::Forms;
	using namespace System::Data;
	using namespace System::Drawing;

	/// <summary>
	/// Сводка для MyForm1
	/// </summary>
	public ref class MyForm1 : public System::Windows::Forms::Form
	{
		// 1. НАШИ ПЕРЕМЕННЫЕ ДОБАВЛЯЕМ СЮДА (внутрь класса)
	private:
		AgeModel* model;
		AgeController* controller;

	public:
		// 2. ИЗМЕНЯЕМ КОНСТРУКТОР (Имя должно быть MyForm1)
		MyForm1(AgeModel* m, AgeController* c)
		{
			InitializeComponent();

			// Сохраняем переданные ядро и контроллер
			model = m;
			controller = c;

			// ВНИМАНИЕ: Восстановление данных. 
			// Раскомментируйте (уберите //) три нижние строчки ТОЛЬКО ПОСЛЕ ТОГО, 
			// как перетащите три текстовых поля на эту форму в визуальном редакторе 
			// (и убедитесь, что они называются textBox1, textBox2, textBox3).

			// textBox1->Text = model->getDay().ToString();
			// textBox2->Text = model->getMonth().ToString();
			// textBox3->Text = model->getYear().ToString();
		}

	protected:
		/// <summary>
		/// Освободить все используемые ресурсы.
		/// </summary>
		~MyForm1()
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
			this->SuspendLayout();
			// 
			// MyForm1
			// 
			this->AutoScaleDimensions = System::Drawing::SizeF(6, 13);
			this->AutoScaleMode = System::Windows::Forms::AutoScaleMode::Font;
			this->ClientSize = System::Drawing::Size(323, 261);
			this->Name = L"MyForm1";
			this->Text = L"Ввод данных";
			this->ResumeLayout(false);

		}
#pragma endregion
	};
}